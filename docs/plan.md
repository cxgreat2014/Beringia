<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Plan

How Beringia gets from "XNU compiles" to "macOS 26 userspace runs on WSL2". The architecture is in
[architecture.md](architecture.md); milestones and their exit criteria are in [../ROADMAP.md](../ROADMAP.md). This
file is the work breakdown: what each milestone needs, in the order it will be done, and how it is verified.

## Principles

1. **Native XNU first.** Use XNU's own x86_64 machine layer (traps, syscalls, pmap, context switch). Patch XNU only
   where it would touch hardware Linux owns (LAPIC, IOAPIC, MTRR/PAT, serial ports, ACPI, power management MSRs) or
   where it needs a host service. Every hosted patch is small, has one topic and is guarded by `beringia_hosted()`.
2. **Real resources.** Real physical memory, real page tables, real descriptor tables. No shadow page tables, no fake
   physical addresses, no trapping of XNU's normal operations.
3. **Linux stays in control.** Interrupts are forwarded to Linux immediately; the host can always end a run.
4. **Verified steps.** A milestone is done when its exit criterion is observed and recorded (log, test output).
5. **Reproducible from public inputs.** Pinned upstreams, scripts for every step, no Apple binaries in the
   repository; the user's own macOS assets are read from `BERINGIA_ASSETS`.

## M1 — Host foundation (`host/`)

| Work item | Notes |
|---|---|
| Carve-out manager | Module parameter `carve=BASE:SIZE[,...]`; refuse ranges that intersect System RAM; `memremap(WB)`; export `beringia_mem_*` |
| Per-CPU world area | Saved Linux state, saved XNU state, XSAVE areas (64-byte aligned), trampoline stack pointers |
| World switch (`host/arch/x86_64/world.S`) | Enter, host call, forwarded interrupt, abort. Switch order: CR3 → GDT/TR (busy-bit handling, Linux RO GDT) → IDT → segments (far return for CS) → MSRs (GS/KGS/FS, LSTAR/STAR/SFMASK/CSTAR, TSC_AUX, S_CET) → XCR0 + XSAVE/XRSTOR |
| Interrupt forwarding table | 256 `int $n` stubs executed in the Linux world |
| Linux PML4 survey | Which kernel-half slots Linux uses (for XNU's physmap/double-map placement); PDPT entries 510/511 of slot 511 |
| Self-test payload | A tiny freestanding "foreign world" (own CR3 copy, GDT with XNU's selector layout, IDT, TSS) that spins with interrupts enabled and counts forwarded interrupts; `selftest=N` runs N round trips |
| CPU threads | Pinned kthreads; enter / leave; abort on unload |

Exit: self-test passes 10⁶ round trips and 10 s of forwarded timer interrupts with no warning in `dmesg`.

## M2 — XNU early boot (`loader/`, `xnu/patches/hosted/`)

Loader:
- Mach-O parse, segment copy to `PA = VA - KERNEL_BASE + slide`, local relocations (+slide), `__HIB` placement.
- `boot_args` (v2): command line, EFI memory map (carve-outs conventional), `kaddr/ksize`, `kslide`,
  `PhysicalMemorySize`, `efiMode = 64`, device tree pointer; no EFI system table.
- Device tree (XNU flattened DT format): `/chosen` (boot args, memory-map entries for ramdisk later), `/efi/platform`
  (FSB/TSC frequency, model), `/cpus`.
- Boot page tables: identity map of the low 4 GiB (slot 0, 1 GiB pages), slot 511 PDPT entries 0–3 → low 4 GiB, plus
  Linux's PDPT entries 510/511.
- Entry: CPU 0 thread → world switch with a temporary GDT (XNU selector layout) → `vstart(boot_args_pa)` on XNU's
  `low_eintstack`.

XNU hosted patches (first set), each one topic:
1. `beringia.c` + ABI header: hosted detection, `beringia_call()`, console `putc` and panic/assert to the host.
2. Boot page tables: `Idle_PTs_init` copies the host's PDPT entries 510/511 into `IdlePDPT`; physmap and double-map
   slots from the host; kernel VA ceiling below Linux's region; non-global mappings in slot 510.
3. Platform probing off: serial ports, ACPI tables, EFI runtime, MTRR/PAT, microcode, MCA banks, `xcpm`/pmCPU MSRs.
4. TSC frequency from the host.
5. LAPIC replaced by a hosted `lapic_ops` (ids from the host, EOI no-op, no timer programming); IOAPIC untouched.
6. PCID off while hosted.

Exit: `kernel_bootstrap_thread` runs; XNU's `printf` reaches `dmesg`.

## M3 — Time, idle, CPUs

- Hosted `rtc_timer` (deadline → host hrtimer), `machine_idle` → host wait, delivery of XNU's timer in the forwarded
  interrupt path and after idle.
- `interrupt()` forwarding of host vectors; NMI/#MC forwarding.
- Yield policy (need_resched in Linux + preemptible XNU context).
- SMP: `ml_processor_register` / start via host; IPIs through pending words and host kicks; TLB shootdowns work
  through XNU's own IPI path.
- Paravirtual spinning: XNU spin-waits call the host after a bound (a CPU thread holding a lock may be descheduled by
  Linux).

Exit: 10-minute run with N CPUs, scheduler and timers active, no RCU stall or soft-lockup warning.

## M4 — Platform and kernel collection

- `kexts/BeringiaPlatform` (`IOPlatformExpert`), `kexts/BeringiaCPU` (`IOCPU`).
- `tools/kc/`: build an x86_64 `MH_FILESET` boot kernel collection from Beringia's kernel + kexts + selected Apple
  kexts from the user's 25A353 `BootKernelExtensions.kc` (extract, re-link with chained fixups, `__PRELINK_INFO`).
- Loader: load a KC instead of a bare kernel (`KC_hdrs_vaddr`).

Exit: IOKit matches the platform, CPUs register, boot stops at "waiting for root device".

## M5 — First macOS 26 userspace

- `kexts/BeringiaDisk`: paravirtual block device (host file, physical-address I/O requests).
- Root: the 25A353 recovery ramdisk (`x86_64SURamDisk.dmg`, APFS) first, as a ramdisk or a disk image.
- AMFI / trust cache: the matching 25A353 trust caches loaded as the booter would; boot-args for development policy
  where needed.

Exit: `launchd` is PID 1 and a shell prints `sw_vers` (26.0, 25A353) on the hosted console.

## M6 — Full system

- `tools/disk/`: rebuild a complete macOS 26.0 system volume from the 25A353 full OTA payload (pbzx + Apple Archive)
  on a POSIX file system, then an APFS (or HFS+ for bring-up) disk image.
- `kexts/BeringiaNet` + host TAP/bridge; `BeringiaConsole`.

Exit: multi-user boot, console login, `ssh` from Linux into macOS.

## M7 — Display and input

- Framebuffer kext + host viewer; input kext; CPU rendering for WindowServer.

Exit: loginwindow/Finder screenshot captured from the host.

## Verification assets

- macOS 26.0 build 25A353 x86_64: `BootKernelExtensions.kc` (kernel `xnu-12377.1.9~3/RELEASE_X86_64`), trust caches
  (`bstc`, `rtsc`), recovery ramdisk, full OTA payload. Fetched by `scripts/assets/` into `BERINGIA_ASSETS`;
  never committed.
- Kernel Debug Kit 25A353 (headers, symbols for comparison).

## Risks

| Risk | Mitigation |
|---|---|
| World-switch bug crashes the WSL VM | Self-test first; `panic_on_oops=0`; host trace to a Windows file; `wsl --shutdown --force` |
| Hyper-V refuses MSRs XNU reads | Hosted patches skip those paths; a host-side MSR-probe log |
| Apple kexts expect real Intel platform services (SMC, ACPI) | Provide minimal paravirtual stand-ins only when a needed kext requires them |
| Upstream LLVM vs Apple clang differences (kalloc_type, iig) | Documented compat patches; revisit with a clang plugin if zone behaviour diverges |
| Single developer bandwidth | Milestones are independent enough to accept contributions (kexts, tools, disk builder) |
