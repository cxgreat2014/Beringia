# Beringia

**Run macOS (x86_64) userspace natively on x86_64 Linux by hosting the XNU kernel inside Linux.**

Beringia builds XNU, the open-source kernel of macOS, from Apple's published sources and runs it as a pair of
loadable Linux kernel modules. The hosted XNU owns its CPU state while it runs — its own page tables, descriptor
tables and system-call entry — so macOS processes execute directly on the same x86_64 CPUs as Linux processes, with
no hypervisor and no emulation in between. Linux lends XNU memory, time, interrupts and devices.

The first target is **WSL2 on an x86_64 Windows PC**, validated with a **macOS 26.0 (build 25A353) x86_64 system
disk**: the last macOS release for Intel Macs, whose kernel is `xnu-12377.1.9`.

> Status: **early bring-up, built in public.** Nothing boots yet. See [ROADMAP.md](ROADMAP.md) for where we are and
> [docs/plan.md](docs/plan.md) for how we intend to get there. 中文说明见 [docs/zh-CN/README.md](docs/zh-CN/README.md)。

The name: Beringia was the land bridge between two continents. Here the continents are two kernels, and the bridge is
a world switch.

## How it works (short version)

```
 macOS processes (launchd, ...)          Linux processes (systemd, sshd, ...)
 ───────────────────────────────         ───────────────────────────────────
 XNU, x86_64, hosted machine layer  │
   ↕ beringia ABI (xnu/include/beringia/abi.h): host calls, forwarded interrupts
 beringia_xnu.ko   (APSL-2.0)  loads XNU, builds its boot environment, dispatches host calls
   ↕ host interface (host/include/beringia/host.h)
 beringia_host.ko  (GPL-2.0)   memory, world switch, interrupt forwarding, timers, CPUs, files
 Linux kernel (x86_64; first target: WSL2)
```

- **One world per CPU at a time.** A hosted XNU CPU is a pinned Linux kernel thread. Entering XNU loads XNU's CR3,
  GDT, IDT, TSS, GS bases, syscall MSRs, PAT and FPU state; returning to Linux restores Linux's. XNU's native x86_64
  trap, syscall and context-switch code runs unmodified.
- **Real physical memory.** XNU gets RAM that Linux never uses (reserved at boot with `memmap=`) and manages it with
  its own pmap, so the page tables it builds are walked by the hardware.
- **Interrupts stay Linux's.** A hardware interrupt that arrives while XNU runs enters XNU's IDT and is handed straight
  back to Linux's handler; XNU's own timer and IPIs are delivered by the host.
- **Devices are paravirtual.** Disk, console, network and display are small IOKit drivers talking to the host.

Details: [docs/architecture.md](docs/architecture.md).

## Repository layout

| Path | License | What |
|---|---|---|
| `host/` | GPL-2.0-only | `beringia_host.ko`: the only code that touches Linux internals |
| `loader/` | APSL-2.0 | `beringia_xnu.ko`: Mach-O/kernel-collection loader, boot environment, host-call dispatch |
| `xnu/` | APSL-2.0 | Patches against `xnu-12377.1.9` (`xnu/series`) and the shared ABI header |
| `kexts/` | APSL-2.0 | Hosted platform, CPU and paravirtual device drivers for XNU |
| `scripts/` | GPL-2.0-only | Toolchain, XNU build, host setup, boot and asset scripts |
| `tools/` | GPL-2.0-only | Mach-O / kernel-collection / disk / debugging tools |
| `tests/` | GPL-2.0-only | Tests |
| `docs/` | GPL-2.0-only | Design, plan, ADRs, setup guides |

Interface headers shared across the license line (`host/include/beringia/host.h`, `xnu/include/beringia/abi.h`) are
MIT. Every file carries an `SPDX-License-Identifier`; see [LICENSE](LICENSE).

## Building (work in progress)

```sh
scripts/toolchain/setup.sh     # Darwin cross toolchain on Linux (cctools/ld64, MIG, SDK, KDK headers)
scripts/xnu/prepare.sh         # fetch xnu-12377.1.9, apply xnu/series
scripts/xnu/fakeroot.sh        # header-only SDK root + libfirehose_kernel
scripts/xnu/build.sh           # kernel.development for x86_64
scripts/host/build-modules.sh  # beringia_host.ko + beringia_xnu.ko against the running kernel
```

Host setup for WSL2 (custom kernel, reserved memory): [docs/setup/wsl2.md](docs/setup/wsl2.md).

## What this repository never contains

No Apple binaries: no kernels, kexts, kernel collections, trust caches, SDKs, KDKs or system disks. Scripts fetch or
read them from your own sources at build time; see [docs/setup/macos-assets.md](docs/setup/macos-assets.md).

## Acknowledgements

- [xnulinux](https://github.com/AFK-surf/xnulinux) — the arm64 project that first ran macOS userspace this way (XNU as
  a Linux module on arm64). Beringia's design follows its proven split between a GPL host module and an APSL XNU-side
  module; where code is adapted from it, the file says so and keeps its license. See [NOTICE.md](NOTICE.md).
- [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) — the Linux-hosted Darwin toolchain and XNU build support.
- Apple's open-source [XNU](https://github.com/apple-oss-distributions/xnu).
