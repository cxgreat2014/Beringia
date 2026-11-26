<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Using and contributing to Beringia

A practical guide to the repository: what each part does, how to build and run it, and where to make changes.

## Components

| Path | Module | Role |
|---|---|---|
| `host/` | `beringia_host.ko` (GPL-2.0) | The only code that touches Linux internals: carve-out RAM, the x86_64 world switch, interrupt forwarding, CPU threads, timers, host files, logging. |
| `loader/` | `beringia_xnu.ko` (APSL-2.0) | Loads XNU (Mach-O, slid headers, local relocations), builds boot_args/EFI map/device tree/page tables, dispatches host calls. |
| `xnu/` | patches + ABI | `xnu/patches/build/` (build on Linux) and `xnu/patches/hosted/` (the BERINGIA machine glue), plus `xnu/include/beringia/abi.h`, the shared ABI. |
| `kexts/` | (APSL-2.0) | Hosted platform/device drivers. `BeringiaPlatform` is currently linked into the kernel directly; more come in M4. |
| `scripts/` | (GPL-2.0) | Toolchain, XNU build, WSL2 kernel and memory reservation, boot, self-test, watchdog. |
| `tools/` | (GPL-2.0) | Build helpers (`iig_fixup.py`), and later the KC and disk builders. |
| `docs/` | (GPL-2.0) | Architecture, plan, ADRs, setup, current issues, debugging, this guide. |
| `tests/` | (GPL-2.0) | Self-test evidence (`tests/results/`). |

The two modules meet **only** at `host/include/beringia/host.h` (MIT), a small C interface of plain integer types and
opaque handles. XNU-side code talks to the host through `xnu/include/beringia/abi.h` (MIT), the `beringia_call` ABI.

## Build once

```sh
scripts/toolchain/setup.sh      # Darwin cross toolchain (cctools/ld64, MIG, SDK, KDK headers)
scripts/host/wsl-kernel.sh      # build the WSL2 kernel (needed for out-of-tree modules with matching vermagic)
scripts/xnu/prepare.sh          # fetch xnu-12377.1.9 and apply xnu/series
scripts/xnu/fakeroot.sh         # header-only SDK root + libfirehose_kernel
scripts/xnu/build.sh            # build the x86_64 kernel
```

Set up `.wslconfig` (memory reservation and a custom kernel) per [setup/wsl2.md](setup/wsl2.md), then `wsl --shutdown`.
Everything Beringia downloads or builds lives under `~/beringia-work` (override with `BERINGIA_WORK`). Machine-local
paths go in `local.env` (git-ignored).

## Build and run each iteration

```sh
make -C host    KDIR=$LINUX_KDIR     # as your normal user
make -C loader  KDIR=$LINUX_KDIR
bash scripts/run/boot.sh             # as root
```

See [debugging.md](debugging.md) for console/panic reading and VM recovery.

## Changing XNU

XNU changes are **patches**, one topic per patch, generated from a git tree, never ad-hoc scripts:

```sh
# $BERINGIA_XNU_SRC is a checkout of xnu-12377.1.9 with the series applied (scripts/xnu/prepare.sh).
#   edit, then commit per topic ("hosted: ..." or "build: ...")
scripts/xnu/export.sh            # regenerate xnu/patches/ and xnu/series from those commits
scripts/xnu/prepare.sh --check   # verify the series applies to a pristine tag
scripts/xnu/build.sh             # rebuild the kernel
```

Hosted changes are guarded by `beringia_hosted()` so a non-hosted build still behaves as stock XNU.

## Changing the host or loader

`host/` (GPL) may use Linux internals but must not include XNU code. `loader/` (APSL) must not use Linux headers or
functions beyond the module ABI and `host.h`. Every source file carries an `SPDX-License-Identifier`
(`scripts/check-spdx.sh` enforces it in CI).

- `host/arch/x86_64/world.S`, `world.c`, `world.h` — the world switch. Read these first if touching CPU state; offsets
  are fixed by hand and checked with `_Static_assert`.
- `host/mem.c` — carve-out claim and physical-address access for host services.
- `host/cpu.c` — CPU threads, idle waits (`bh_idle`), kicks, one-shot timers (`bh_timer_set`).
- `loader/macho.c` — Mach-O parse, segment placement, local relocations, header slide.
- `loader/boot.c` — boot_args, EFI memory map, the flattened device tree, boot page tables.
- `loader/hostcalls.c` — the `enum beringia_call` dispatch.

## The host-call ABI in one page

XNU calls the host via `beringia_hook(nr, a1, a2)` (SysV: `rdi=nr, rsi=a1, rdx=a2`, `rax=result`). The host switches
to the Linux world for the call's duration and dispatches `ldr_hostcall`. Calls used today: `CONSOLE`, `PANIC`, `HALT`,
`MARK`, `FORWARD_IRQ`, `SET_TIMER`, `IDLE`, `IPI`, `YIELD`, `WALLCLOCK`, `RANDOM`. Numbers and the shared structures
(`beringia_shared`, per-CPU pending words) are in `xnu/include/beringia/abi.h`.

## What to read next

- [architecture.md](architecture.md) — the two-world model, memory layout, interrupt model.
- [current-issues.md](current-issues.md) — the active timer-delivery blocker and every known limitation.
- [debugging.md](debugging.md) — how to boot, read panics, symbolize, and recover.
- [plan.md](plan.md) and [../ROADMAP.md](../ROADMAP.md) — the milestone breakdown.
- [adr/](adr/) — why the design is this way (real physical memory, full world switch, interrupt forwarding, …).

## Ground rules for contributions

- No Apple binaries in the repository (kernels, kexts, KCs, trust caches, SDKs, disk images).
- Respect the license line (`host/` GPL, `loader/`/`xnu/`/`kexts/` APSL, interface headers MIT).
- A milestone is done when its exit criterion is observed and recorded (a log line, a test).
- Decisions with real alternatives get an ADR in `docs/adr/`.
