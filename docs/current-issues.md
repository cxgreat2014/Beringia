<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Current implementation issues and open work

An honest account of where Beringia stands, what is broken, and what is next. Every "reaches X" below names the run
directory (`$BERINGIA_WIN_DIR/runs/<id>/`, see [debugging.md](debugging.md)) that shows it. For the architecture, see
[architecture.md](architecture.md); for milestones, [../ROADMAP.md](../ROADMAP.md).

## Where XNU gets to today (2026-11-26, runs 20261126-233250, 20261126-231845)

**M4 is done** (security stack, 14-minute clean run), and **M5 has started: real Apple userland runs.**

- The loader boots a RAM-disk root: `ramdisk=` module parameter, the image placed after the boot data inside the
  extent `boot_args` reports (`kaddr + ksize`, so XNU's early page tables go after it), memdev `pmd0=` + `rd=md0`.
  `scripts/ramdisk/build.sh` builds a small HFS+ test root with a static init; `tests/host/` has the x86_64
  memcheck/fpcheck isolation tests (both pass on a quiet host and detect deliberate corruption).
- Filesystems come from the KDK, not the kernel source (open-source XNU has no `bsd/hfs`): `HFS.kext` mounts the
  test HFS+ root; `apfs.kext` (+ Zlib/Dataless) mounts Apple's `x86_64SURamDisk.dmg` recovery image.
- With the full security stack and APFS, **Apple's real launchd executes** from the recovery ramdisk (run
  20261126-233250): APFS mounts, `load_init_program` runs `/sbin/launchd`, and it prints the Darwin Ignition
  Sequence banner. It then stops progressing in ignition (next thing to investigate: it likely wants the boot
  environment/system volumes a real boot provides).
- A self-written unsigned `/sbin/launchd` is rejected by AMFI ("The system only allows platform binaries"): use
  Apple-signed userland, or expect to work around AMFI for custom test programs.

## Fixed today (and why the VM used to wedge)

| Problem | Root cause | Fix |
|---|---|---|
| The whole WSL VM wedged the first time XNU idled | The hosted LAPIC uses the xAPIC ops on a RAM page, so `is_x2apic == FALSE`. `lapic_configure()` → `lapic_reinit()` read the *real* `IA32_APIC_BASE`, saw the host's x2APIC mode and called `legacy_init()`, which disables the real local APIC and re-enables it in xAPIC mode. That CPU never received another interrupt; Linux stalled waiting for its IPIs. | XNU patch "never reprogram the host's local APIC" |
| XNU writes CPU state the world switch does not switch | `pat_init`/MTRRs (with CR0.CD + `wbinvd`), the AP trampoline at physical 0x2000 (host RAM), MCA banks, TSX/SRBDS MSRs, PMU fixed counters | XNU patch "leave the host's memory types, MCA banks, errata MSRs and PMU alone"; PCIDs disabled in code |
| A timer delivered by a forwarded interrupt was missed | `interrupt()` read the pending word before forwarding | read after forwarding, atomically |
| `kernel_bootstrap_thread` blocked forever in `ml_wait_max_cpus` | nobody called `ml_set_max_cpus` (normally the ACPI CPU interrupt controller) | BeringiaPlatform announces the host's CPU count |
| 2 × 30 s waits in `IOKitInitializeTime` | no IORTC, no IONVRAM | BeringiaPlatform publishes both; time of day from the host |
| `trust_cache_init` panic | no AppleImage4/AMFI in a bare kernel | bring-up stub (no trust caches) until the KC brings AMFI |
| idle wait of 4 s instead of 4 ms | unit error in `bh_idle` | fixed |
| boot-args with spaces lost | the kernel splits module parameters at spaces | `boot.sh` passes `boot_args="..."` |

## Safety nets now in place

- **State safety net** (`beringia_host.ko statecheck=1`, default): after every exit from the foreign world, CR4, CR8,
  DR7, IA32_PAT, EFER, IA32_APIC_BASE, DEBUGCTL, MTRR_DEF_TYPE, MISC_ENABLE and the PMU controls are compared with
  their values at entry; differences are logged once and restored. A changed IA32_APIC_BASE cannot be repaired and
  ends the run.
- **Self-test** covers sleeping inside a host call (`selftest_sleep`) and the safety net (`selftest_tamper`).
- **Thread dump** (`echo 1 > /sys/module/beringia_xnu/parameters/dump`) and `tools/debug/symbolize.py`.
- **Run directories, host-side console log and the watchdog**: [debugging.md](debugging.md).

## Known limitations and deliberate bring-up choices

| Area | State |
|---|---|
| kexts | A legacy prelinked kernel with the security stack (28 kexts; `tools/kc`, `scripts/kc/build.sh --closure`). BeringiaPlatform is still linked into the kernel. |
| trust caches | `trust_cache_init` succeeds with the img4 interface; no static trust cache is supplied by the loader yet (needed for real signed userland). Patch 0029 remains as the no-image4 fallback. |
| kalloc_type | Degraded to size-based data zones (upstream LLVM has no `__builtin_xnu_type_summary`). Kexts' own views are not parsed at startup in the static format and go through `kalloc_type_impl_external`'s fallback (as kexts outside the BootKC do on macOS): no hang, verified with corecrypto and pthread. |
| LAPIC | A RAM page with no-op init; timer and IPIs go through the host. |
| NMI / #MC | **Not forwarded yet**: in the foreign world they enter XNU's own handlers. |
| NVRAM | IODTNVRAM is a stub; IONVRAM is announced as ready. |
| SMP | One hosted CPU. The pending word is atomic on both sides already. |
| Physical memory | XNU's physmap maps all of RAM, the host's included, writable. Restricting it to the carve-outs is open. |
| Linux latency | While XNU runs with interrupts off, that Linux CPU takes no interrupts. Measure before SMP. |

## Next steps, in order

1. **M5, first userspace**: the loader boots a RAM-disk root (`ramdisk=` module parameter, memdev `pmd0=` + `rd=md0`,
   patch 0033), with a static test init; then a real root volume and `launchd`.
2. A host-backed block device (BeringiaDisk) and the APFS system volume for the real userland.
3. `tools/kc` tests in CI; host isolation tests (memcheck/fpcheck) after boots; NMI / #MC forwarding.
