<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Current implementation issues and open work

An honest account of where Beringia stands, what is broken, and what is next. Every "reaches X" below names the run
directory (`$BERINGIA_WIN_DIR/runs/<id>/`, see [debugging.md](debugging.md)) that shows it. For the architecture, see
[architecture.md](architecture.md); for milestones, [../ROADMAP.md](../ROADMAP.md). The detailed M5 experiment log is
[research/m5-userspace-bringup.md](research/m5-userspace-bringup.md).

## Where XNU gets to today (2026-11-27)

**M4's boot path works** (security stack, 14-minute clean run), and **M5 has real Apple services running from a
host-backed disk** — but there is no shell, no installed full system, and no GUI yet.

- The legacy prelinked kernel currently links **35 kexts**: 34 from the KDK closure (security, storage and APFS)
  plus `BeringiaDisk`. `tools/kc/deps.py` computes the closure, honouring `OSBundleLibraries_x86_64`.
- `BeringiaDisk` exposes a raw host file as an `IOBlockStorageDevice` through the block-info/read/write/flush host
  calls. With `rd=disk0`, Apple's `x86_64SURamDisk.dmg` is found as boot media and its APFS volume mounts.
- The recovery **launchd completes the ignition sequence and starts `ramrod` and `syslogd`** (run
  `20261127-012220`). This validates exec, dyld, initial IPC and service startup, not a complete macOS install.
- `ramrod` then reports `MSU 1008 (an error occurred finding the target volume)` and reboots deliberately. That is
  the current stopping point: only the recovery image is present; there is no target System volume.
- A self-written unsigned `/sbin/launchd` is rejected by AMFI ("The system only allows platform binaries"). The
  current successful userland is therefore Apple-signed recovery software.
- The full-system OTA tree is extracted, but its runtime is incomplete: `libSystem.B.dylib`,
  `System/Library/dyld/dyld_shared_cache_x86_64`, OS cryptex contents and a full-system static trust cache are still
  required before a normal multi-user boot.

## Important fixes in this round

| Problem | Root cause | Fix |
|---|---|---|
| launchd waited forever after the ignition banner | `hw.osenvironment` is released by `registerNVRAMController`, which the hosted NVRAM stub bypasses | hosted patch 0035: BeringiaPlatform calls `sysctl_unblock_osenvironment()` |
| launchd died with `EXC_BAD_ACCESS`, code `0xd 0` after ignition | Linux left **CR4.UMIP** set; macOS executes `sidt` in userspace in `__os_cpu_number`, which then raises #GP | `world_asm.S` clears CR4.UMIP on every foreign entry (commit `403fa76`); Linux's CR4 is restored on exit |
| registry matching crashed in `ramrod` | `dictionaryWithProperties()` dereferenced a missing property table | hosted patch 0038 returns an empty dictionary |
| XNU could write the host's RAM through its physmap | every physical page up to `phys_top` was mapped writable | hosted patch 0037 (P2): only conventional EFI ranges, exactly the carve-outs, are mapped |
| no disk for a full system | RAM-disk images are limited by the carve-out | `BeringiaDisk` and the loader's `disk=PATH` backend, with `BERINGIA_DISK` forwarded by `boot.sh` |

The detailed boot arguments and reproduction commands are in [research/m5-userspace-bringup.md](research/m5-userspace-bringup.md).

## Earlier host-stability fixes

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
- **Restricted physmap**: only the carve-outs are mapped; an access to unrelated host RAM faults instead of silently
  corrupting it.
- **Self-test** covers sleeping inside a host call (`selftest_sleep`) and the safety net (`selftest_tamper`).
- **Thread dump** (`echo 1 > /sys/module/beringia_xnu/parameters/dump`), `tools/debug/symbolize.py`, and fatal
  userspace exception logging (hosted patch 0036).
- **Run directories, host-side console log and the watchdog**: [debugging.md](debugging.md).
- **Host isolation tests**: `tests/host/` memcheck/fpcheck pass on a quiet host and detect deliberate corruption.

## Known limitations and deliberate bring-up choices

| Area | State |
|---|---|
| Userland | Recovery launchd and initial services run. No interactive shell, console login, multi-user boot or GUI. |
| Full-system runtime | The OTA base system alone is insufficient; the Intel runtime/cryptex and full static trust cache are still needed. |
| kexts | A legacy prelinked kernel (35 kexts in the current disk build). BeringiaPlatform is linked into the kernel; a standalone BeringiaCPU kext is not done. |
| trust caches | `trust_cache_init` succeeds with the img4 interface; no full-system static trust cache is supplied by the loader yet. Patch 0029 remains as the no-image4 fallback. |
| kalloc_type | Degraded to size-based data zones (upstream LLVM has no `__builtin_xnu_type_summary`). Kext views use the external fallback in the static format; no hang has been observed. |
| Block I/O | BeringiaDisk mounts APFS and reads successfully. Full write correctness and the read-only fallback are not yet validated; flush orders host file operations but is not an `fsync` to stable storage. |
| LAPIC | A RAM page with no-op init; timer and IPIs go through the host. |
| NMI / #MC | **Not forwarded yet**: in the foreign world they enter XNU's own handlers. |
| NVRAM | IODTNVRAM is a stub; IONVRAM is announced as ready. NVRAM writes from ramrod report unsupported function. |
| SMP | One hosted CPU. The pending word is atomic on both sides already. |
| Linux latency | While XNU runs with interrupts off, that Linux CPU takes no interrupts. Measure before SMP. |

## Next steps, in order

1. Complete the matching Intel runtime and build the System/Data volumes; boot them through BeringiaDisk and record
   the next concrete userland checkpoint.
2. Add console input and reach a usable shell; validate block writes and durable flush before relying on a writable
   system volume.
3. Then networking, display/input and WindowServer. Keep NMI/#MC forwarding, SMP and `tools/kc` CI tests as separate
   hardening work rather than claiming they are done.
