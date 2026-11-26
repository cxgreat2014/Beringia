<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Current implementation issues and open work

An honest account of where Beringia stands, what is broken, and what is next. For the architecture that is already
proven, see [architecture.md](architecture.md) and the milestone list in [../ROADMAP.md](../ROADMAP.md).

## Where XNU gets to today

`beringia_host.ko` + `beringia_xnu.ko` boot the hosted XNU (`xnu-12377.1.9`, macOS 26.0) on WSL2 through:

- `vstart`, `i386_init`, the entire memory subsystem (pmap bootstrap, VM bootstrap, zones),
- scheduler initialization, `kernel_bootstrap_thread`,
- `PE_init_iokit`: `BeringiaPlatform` (an `IODTPlatformExpert`) matches the boot device tree and IOKit creates the
  device-tree nubs and reaches `IOServicePH` system-capability changes.

Everything is on real hardware: real physical memory for XNU, its own CR3/GDT/IDT/TSS/MSRs while it runs, and Linux
interrupts forwarded into it. The host's self-test (`scripts/run/selftest.sh`) proves the world switch at 10M+ round
trips and tens of thousands of forwarded interrupts with no Linux warning.

## Active blocker: the XNU timer is not being delivered

**`rtclock_intr` is never called** (an instrumented heartbeat in `rtclock_intr` never fires). Consequences: IOKit's
workloops have no clock and stall during capability changes instead of reaching "waiting for root device".

What the bring-up diagnostics established (this is why a bare "it does not boot" would be misleading):

- `interrupt()` is not being called for external interrupts (`FORWARD_IRQ` hostcalls never happen).
- `lapic_set_tsc_deadline_timer` is not being reached to arm the host timer (`SET_TIMER` hostcalls never happen).
- The host-side `bh_idle()` previously never returned, which parked the hosted CPU in `machine_idle` with interrupts
  off. A bounded wait was added (return after ~4 ms) to stop that class of hang.
- The kernel *does* reach `machine_idle`, and `setPop` *does* select `rtc_lapic_set_tsc_deadline_timer` as the
  timer's `rtc_set` implementation (confirmed by disassembly: it calls `lapic_set_tsc_deadline_timer`, whose hosted
  branch calls `beringia_call(SET_TIMER)`).

So the delivery chain `lapic_set_tsc_deadline_timer → SET_TIMER → host hrtimer → PENDING_TIMER → interrupt() →
rtclock_intr` has a break somewhere, and the next debugging step is to find which link never runs. The instrumented
hostcalls (`hc nr=…`) currently show only console and idle traffic.

This is the single most important thing to fix before IOKit can proceed. It is **not** a fundamental design problem —
the world switch and interrupt forwarding are already proven by the self-test — but a bring-up gap in how XNU's
timer/LAPIC programming is routed to the host.

## Known limitations and deliberate bring-up choices

| Area | State |
|---|---|
| kalloc_type | Degraded to size-based data zones. Upstream LLVM has no `__builtin_xnu_type_summary` (Apple-clang-only), so typed segregation is unavailable. Same approach as the macOS-DroidVM Linux bring-up. A faithful replacement (clang plugin) is open work. |
| LAPIC | XNU gets a fake MMIO page (legacy MMIO ops). It never touches the host's real APIC. Timer/IPI delivery must go through the host, not the fake page. |
| TSC-deadline MSR | Routed to the host (`lapic_set_tsc_deadline_timer`), never written directly by XNU (Hyper-V would fault). |
| `machine_idle` | Goes through the host (`bh_idle`), bounded so the CPU cannot park forever. |
| Random numbers | Before corecrypto/prng_funcs register, `read_random`/`cc_rand_generate` fall back to the host CSPRNG. |
| `IODTNVRAM` | A stub (no NVRAM on the hosted platform). `BeringiaPlatform::savePanicInfo` is a no-op so a panic cannot wedge the VM committing a log to NVRAM. |
| `getProperty` | Returns NULL for a nub with no property table (would otherwise call `getObject` on NULL). |
| kexts / kernel collection | Not built yet. Only BeringiaPlatform is linked in statically. Apple kexts from the 25A353 BootKC and a KC builder (`tools/kc/`) are M4 work. |
| SMP | Single hosted CPU only (`linux_cpu=1` by default). Multi-CPU start and IPIs are M3 work. |
| Devices | No disk, network, display, or input kexts yet. No userspace yet. |

## Next steps, in order

1. **Timer delivery (M3)** — find why `lapic_set_tsc_deadline_timer` and `interrupt()` never run, so `rtclock_intr`
   fires and IOKit proceeds. See [debugging.md](debugging.md) for the instrumentation already in place.
2. **IOKit to "waiting for root device" (M4)** — once the clock works.
3. **Platform + kernel collection (M4)** — `BeringiaDisk`, `BeringiaCPU`, and a KC builder for the 25A353 Apple kexts.
4. **First userspace (M5)** — boot `launchd` from the 25A353 recovery ramdisk and print `sw_vers`.
5. SMP, full system volume, display/input, then productization.

## Build-system notes

- **Use the unstripped kernel** (`kernel.development.unstripped`). `strip` zeroes the local-relocation table the
  loader needs, which otherwise fails with "relocation 0 has bad length 0".
- **Force a clean module rebuild after editing.** Incremental kbuild has missed edits (stale `lapic_native.o`, stale
  loader objects). When in doubt: `make -C host clean && make -C loader clean` before rebuilding, and `git clean -fdx`
  is *not* safe to run blindly (it would delete the local `local.env` and the work directories).
- `scripts/xnu/prepare.sh --check` applies the patch series for real (not `git apply --check`), because a file created
  by one patch must exist for a later patch that modifies it.
