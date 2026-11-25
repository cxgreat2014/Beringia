<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Review of a prior x86_64 attempt (November 2026)

Before Beringia, an x86_64 port was attempted inside a fork of xnulinux. This record explains what was kept and why
the rest was not continued, so the same paths are not taken again.

## What it reached

- XNU (`xnu-12377.1.9`, and earlier `xnu-11417.140.69`) compiled for x86_64 on Linux with the macOS-DroidVM toolchain.
- XNU functions (`strlen`, `crc32`, `kprintf`) called from a Linux module on the WSL2 CPU.
- With many patches, early boot reported progress up to `PE_init_iokit`, then zone-metadata panics and a missing
  platform expert.

## What was kept

| Item | Where it is in Beringia |
|---|---|
| The Linux build of x86_64 XNU (DroidVM build support, LLVM compatibility, two stand-in private headers) | `xnu/patches/build/` (verified on a pristine tag) |
| Stack alignment: Linux runs on 8-byte-aligned stacks, XNU code uses aligned SSE stores (16 bytes) | world-switch entry aligns the stack |
| `panic_on_oops=0` and synchronous trace files on the Windows side for crash forensics | `docs/setup/wsl2.md` |
| `wsl --shutdown --force` when the VM wedges | `scripts/run/wsl-recover.sh` |
| Version pairing: 25A353 ↔ `xnu-12377.1.9` (from the KDK and the boot KC) | ADR 0006 |
| Acquisition and verification of the 25A353 Intel assets (BootKC, trust caches, recovery ramdisk, OTA payload) | `scripts/assets/`, `docs/setup/macos-assets.md` |

## Why the rest was not continued

| Finding | Consequence |
|---|---|
| XNU's RAM used **fake physical addresses** (from 32 MiB), and XNU's mappings were copied into Linux's page tables on page faults. XNU never loaded its own CR3. | XNU's page tables cannot be used by the MMU, so there can be no user address spaces. Every kernel mapping costs a fault; stale mappings are never invalidated. |
| The physmap was forced to `VM_MIN_KERNEL_ADDRESS + pa`. | The physmap alias of pa 0x200000 *is* the kernel's `__TEXT`. This only worked because RAM started at 32 MiB; a kernel image above 32 MiB overlaps it. |
| XNU was not allowed to load its GDT, IDT or TSS, and the double map was unlinked. | Selector clash: XNU's `KERNEL64_CS` (0x08) is a 32-bit segment in Linux's GDT and `USER64_CS` (0x2b) is Linux's `__USER_DS`. Any `iretq`/`sysret` with XNU's selectors faults, so there is no path to user space. |
| XNU's per-CPU segment was moved from GS to FS (`idt64.s`, `cswitch.s`). | A deep fork of XNU's entry code. It is unnecessary once XNU owns its descriptor tables. |
| AppleACPIPlatform and friends were statically linked into the kernel, posing as an iMac20,1. | Those kexts drive real ACPI, HPET, IOAPIC, PCI and SMC hardware. See ADR 0005. |
| No model for timers, preemption or host interrupts while XNU runs (interrupts off for the whole call). | RCU stalls and soft lockups on longer runs; XNU's clock stops. |
| 256 MiB of RAM, one CPU, a single global world slot. | Not a foundation for SMP or real workloads. |
| About 100 one-off patch scripts, 188 debug markers inside XNU, an exported patch out of sync with the tree, nothing committed. | Not reproducible. |

## Lessons

1. Decide the memory and address-space model first; everything else depends on it.
2. Prefer running XNU's native machine layer over rewriting it; pay for that with a careful world switch.
3. Keep patches small and topical, generated from a git tree (`git format-patch`), never from ad-hoc scripts.
