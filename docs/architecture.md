<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Architecture

This document describes how Beringia hosts an x86_64 XNU kernel inside a running x86_64 Linux kernel. It is the
reference for the code; decisions with alternatives worth recording are in [adr/](adr/).

## 1. Goals and non-goals

Goals:

- macOS 26 (x86_64) userspace runs **natively** on the host's CPUs: no hypervisor, no binary translation, no
  instruction emulation. A macOS process and a Linux process can run on the same core one after the other.
- The XNU that runs is built from Apple's published source (`xnu-12377.1.9`, the macOS 26.0 kernel) with a
  **small, reviewable** set of patches. XNU's native x86_64 machine layer (traps, syscalls, pmap, context switch)
  is used as-is wherever possible.
- Linux stays in charge of the machine: it owns interrupts, timers, devices and scheduling, and it survives a crash
  of the hosted kernel.
- First host: **WSL2** (Linux 6.18 under Hyper-V) on an Intel x86_64 PC. Bare-metal x86_64 Linux should work the
  same way.

Non-goals (for now): running on AMD-specific paths, sharing GPUs, Secure Boot / signed modules, macOS releases other
than 26.x x86_64.

## 2. Components

```
            ┌──────────────── XNU world ─────────────────┐
            │ macOS userspace (ring 3, XNU's user pmaps)  │
            │ XNU kernel (ring 0): native x86_64 layer +  │
            │   xnu/patches/hosted (BERINGIA machine glue)│
            │ kexts/: BeringiaPlatform, CPU, Disk, ...    │
            └───────────────┬─────────────────────────────┘
                            │ beringia ABI: host calls, forwarded interrupts
            ┌───────────────┴─────────────────────────────┐
            │ beringia_xnu.ko (loader/, APSL-2.0)         │
            │   Mach-O / KC loading, relocation, boot     │
            │   args, device tree, host-call dispatch     │
            └───────────────┬─────────────────────────────┘
                            │ host interface (host/include/beringia/host.h)
            ┌───────────────┴─────────────────────────────┐
            │ beringia_host.ko (host/, GPL-2.0-only)      │
            │   carve-out RAM, world switch, interrupt    │
            │   forwarding, CPU threads, timers, files    │
            └───────────────┬─────────────────────────────┘
                            │ Linux internals
                     Linux kernel (x86_64)
```

The two modules exist because of licensing, as in xnulinux: code that is derived from or links against XNU is APSL;
code that uses Linux internals is GPL. They meet only at `host.h`, a small, versioned C interface of plain integer
types and opaque handles.

## 3. The two worlds

At any instant a CPU runs in exactly one **world**:

| State | Linux world | XNU world |
|---|---|---|
| CR3 | Linux's (`loaded_mm`) | XNU's (`IdlePML4` or a user pmap) |
| GDTR / TR | Linux per-CPU GDT, Linux TSS | XNU's GDT and TSS (`cpu_desc_load`) |
| IDTR | Linux IDT | XNU IDT (`idt64.s`, via the double map) |
| CS / SS | `__KERNEL_CS` (0x10) / `__KERNEL_DS` | `KERNEL64_CS` (0x08) / `KERNEL64_SS` |
| GS_BASE / KERNEL_GS_BASE / FS_BASE | Linux per-CPU / user | XNU `cpu_data` / user TSD |
| LSTAR / STAR / SFMASK / CSTAR | Linux syscall entry | XNU's (`hi64_syscall`) |
| IA32_PAT, TSC_AUX | Linux's | Linux's PAT (XNU does not reprogram it), XNU's TSC_AUX |
| XCR0, FPU/XSAVE state | Linux's | XNU's (saved/restored with XSAVE) |
| CR0 / CR4 | Linux's | CR0: XNU's (switched). CR4: Linux's; XNU must not change it, and the state safety net checks |
| S_CET / PL0_SSP | Linux's | 0 (XNU has no CET support) |

The selector clash alone (XNU's `KERNEL64_CS` 0x08 is a 32-bit code segment in Linux's GDT; XNU's `USER64_CS` 0x2b is
Linux's `__USER_DS`) rules out running XNU on Linux's descriptor tables. A full switch is the price of running XNU's
native trap and syscall code unchanged, and it is a few microseconds.

**Transitions** happen in exactly three places, all in `host/` assembly:

1. **Entry** — a hosted CPU's kernel thread enters XNU (`vstart` for the boot CPU, the AP entry for others) and does
   not return; the thread *is* that XNU CPU from then on.
2. **Host call** — XNU calls `beringia_hook(call, arg)`. The trampoline saves XNU's state, loads Linux's, switches to
   the CPU thread's own Linux stack, runs the call in ordinary kernel-thread context (it may sleep), and switches back.
3. **Forwarded interrupt** — a hardware interrupt, NMI or machine check that arrives in the XNU world enters XNU's IDT;
   XNU's (patched) `interrupt()` immediately makes a host call that switches to the Linux world and re-delivers the
   vector there with `int $vector`, so Linux's own handler runs and sends the EOI. Back in the XNU world, XNU delivers
   whatever the host marked pending for it (its timer, IPIs from other XNU CPUs), in a real interrupt context, and its
   normal return path handles ASTs and preemption.

Every Linux→XNU transition re-captures the Linux state at that moment (Linux may have changed CR3 or the FPU owner
while it ran), so nothing is assumed stable across a host call.

## 4. Memory

### 4.1 Physical memory

XNU's RAM is **real physical memory that Linux never uses**: ranges reserved on the kernel command line with
`memmap=SIZE$BASE` (see [setup/wsl2.md](setup/wsl2.md)). `beringia_host.ko` verifies that a range is absent from
Linux's System RAM, maps it write-back for host-side access, and hands it to the loader. XNU's pmap manages it with its
own physical addresses: the page tables it builds are walked by the MMU as they are.

Native x86_64 XNU assumes its kernel image sits at `physical = virtual - KERNEL_BASE` in the low 4 GiB (booter KASLR
slides both by the same amount). The loader keeps that invariant: it places the kernel at physical `0x200000 + S` and
virtual `0xffffff8000200000 + S` with the slide `S` = the low carve-out's base, applies the kernel's local
relocations, and reports `kslide = S` in the boot arguments. The low carve-out must therefore be below 4 GiB and its
kernel + early allocations below ~1 GiB (the reach of XNU's bootstrap `KPTphys`). Further carve-outs can be anywhere.

The EFI memory map given to XNU lists the carve-outs as conventional memory and the kernel / boot data as loader data;
everything else is absent.

### 4.2 Virtual address space

x86_64 has one kernel half shared by both kernels. Linux randomizes its regions (direct map, vmalloc, vmemmap) within
PML4 slots 273–507 and uses 508 (cpu_entry_area), 510 (espfix) and 511 (kernel image, modules, fixmap at PDPT
entries 510–511). XNU uses slot 511 (kernel, PDPT entries 0–509), 510 (kext "basement"), the physmap slots just below
510 and a double-map slot.

- **Slot 511 is shared at PDPT granularity.** XNU's `IdlePDPT` carries Linux's PDPT entries 510 and 511, so module
  code (the trampolines) and module data (the per-CPU world structures) are mapped in every XNU address space. XNU's
  kernel VA ceiling is lowered below `0xffffffff80000000` so it never allocates there.
- **Slot 510** means the XNU kext basement in XNU's page tables and Linux's espfix in Linux's. The two are never live
  in the same world. XNU's mappings there are made non-global so they cannot survive a switch in the TLB.
- **Physmap / double map** slots are chosen at boot from slots Linux does not use, and passed to XNU.
- **Linux's view of XNU memory** is through physical addresses only (the carve-out mapping). Host calls exchange
  physical addresses, like DMA from a device: no XNU virtual address needs to be valid in Linux's page tables.
- **PCID**: XNU runs with its PCID support disabled (PCID 0 only, full flush on its CR3 writes); Linux's PCIDs are
  never used by XNU's translations.

## 5. Time, interrupts, idle and scheduling

- **Clock**: XNU's timebase is the TSC (shared, invariant under Hyper-V). Its frequency comes from the host (`tsc_khz`)
  because CPUID leaves 0x15/0x16 are not reliable in a VM.
- **Timer**: XNU never touches the LAPIC. Its deadline (`rtclock` / `setPop`) is a host call that arms a pinned
  high-resolution timer on the CPU's Linux side. The hrtimer's interrupt is forwarded like any other; afterwards XNU
  runs `rtclock_intr` if its deadline has passed.
- **Idle**: `machine_idle` calls the host, which sleeps the CPU thread until XNU's next deadline or an IPI kick. Linux
  runs other work on that CPU meanwhile.
- **Fairness**: while XNU is busy, Linux's tick still interrupts it (when XNU has interrupts enabled). After forwarding,
  if Linux wants to reschedule and the interrupted XNU context is preemptible, the host yields the CPU thread (an RCU
  quiescent state, other Linux tasks run).
- **IPIs between XNU CPUs**: the sender sets the target's pending word and asks the host to kick the target's Linux
  CPU (wake its idle wait or send a reschedule IPI, which is forwarded and followed by XNU's `cpu_signal_handler`).
- **NMI / #MC** are meant to be forwarded to Linux (XNU's own NMI-based facilities, panic NMIs, are then
  unavailable). **Not implemented yet**: in the foreign world they currently enter XNU's own handlers.
- **State XNU must leave alone.** CR4, CR8, DR7, IA32_PAT, EFER, IA32_APIC_BASE, DEBUGCTL, MTRRs, MISC_ENABLE and
  the PMU are not switched. XNU's code paths that program them (LAPIC init, PAT/MTRR, MCA, CPU errata workarounds,
  monotonic) are disabled when hosted, and the host checks after every foreign-world exit that they are
  unchanged (`statecheck`): differences are restored and logged; a changed IA32_APIC_BASE ends the run, because
  a local APIC that was disabled has lost its configuration and the CPU no longer takes interrupts.

## 6. CPUs

Each hosted XNU CPU is a kernel thread bound to one Linux CPU. The boot CPU enters `vstart`; the others are started by
XNU (`ml_processor_register` → host call) and enter XNU's slave start path. Topology (one logical CPU per thread,
APIC ids from the host) is presented to XNU through the device tree and boot arguments instead of ACPI.

## 7. Platform and devices

x86 XNU normally gets its platform expert, interrupt controller and timers from `AppleACPIPlatform`. Beringia
replaces that whole layer with its own kexts (`kexts/`):

- `BeringiaPlatform` — an `IOPlatformExpert` for the hosted machine (no ACPI, no PCI, no SMC).
- `BeringiaCPU` — `IOCPU` instances backed by host calls.
- `BeringiaDisk` — a paravirtual block device backed by a host file (the macOS 26 system disk image).
- `BeringiaConsole`, `BeringiaNet`, `BeringiaFramebuffer`, `BeringiaInput` — later milestones.

Apple's own kexts (corecrypto, AMFI, Sandbox, APFS, IOStorageFamily, ...) come from the user's macOS 26.0 (25A353)
kernel collection and are linked with Beringia's kernel and kexts into a new boot kernel collection by
`tools/kc/`.

## 8. Boot sequence

1. Linux boots with the carve-outs reserved (`memmap=`).
2. `insmod beringia_host.ko carve=...` — claims the carve-outs, sets up per-CPU world areas.
3. `insmod beringia_xnu.ko kernel=... cpus=N boot_args=...` — loads and relocates the kernel (later: the kernel
   collection), builds `boot_args`, the EFI memory map, the device tree and the boot page tables, then starts CPU 0's
   thread, which enters `vstart` in the XNU world.
4. XNU boots natively: `vstart` → `i386_init` → `kernel_bootstrap` → IOKit → `launchd`.
5. Stopping: XNU halts or panics → host call → every CPU thread leaves the XNU world through its abort path; the
   modules unload and the carve-outs are free again.

## 9. Failure containment

- A panic or failed assertion in XNU is a host call that ends the run cleanly.
- An exception XNU cannot handle ends in XNU's own panic path (it owns its IDT), not in a Linux oops.
- A world-switch bug, by contrast, can take Linux down; the world-switch code is small, written in assembly with
  explicit invariants, and covered by a self-test (`host/` selftest mode) that runs it against a tiny test payload
  before any XNU code is executed.
- WSL2 recovery: `wsl --shutdown --force` (see [setup/wsl2.md](setup/wsl2.md)).

## 10. Relation to xnulinux

[xnulinux](https://github.com/AFK-surf/xnulinux) runs arm64 XNU inside arm64 Linux. Beringia follows its overall
design (GPL host module + APSL XNU module, host calls, kernel threads as CPUs, paravirtual kexts) but not its
mechanisms, which are arm64-specific (banked TTBR0/TTBR1, VBAR swapping for EL0, PAC keys). The x86_64 equivalents are
described above; the reasons are in [adr/](adr/).
