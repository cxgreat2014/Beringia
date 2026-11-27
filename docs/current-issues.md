<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Current implementation issues and open work

An honest account of where Beringia stands, what is broken, and what is next. Every "reaches X" below names the run
directory (`$BERINGIA_WIN_DIR/runs/<id>/`, see [debugging.md](debugging.md)) that shows it. For the architecture, see
[architecture.md](architecture.md); for milestones, [../ROADMAP.md](../ROADMAP.md). The detailed M5 experiment log is
[research/m5-userspace-bringup.md](research/m5-userspace-bringup.md).

## Where XNU gets to today (2026-11-27, after the audit)

**The full macOS 26 system boots past launchd's early boot and runs ~130 Apple daemons from a 40 GiB APFS system
image on BeringiaDisk**, the IOKit registry goes quiet, and WindowServer is launched (runs `20261127-153054`,
`20261127-161735`), with no kernel panic and no host warning. There is still no display, no input, no shell and no
GUI.

- The legacy prelinked kernel links **39 kexts**: [scripts/kc/full-system.kexts](../scripts/kc/full-system.kexts)
  (security, storage, APFS, AppleSMC, DSMOS, IOSerialFamily/IOUserSerial) plus `BeringiaDisk` and `BeringiaSMC`;
  the recipe is in [research/m5-userspace-bringup.md](research/m5-userspace-bringup.md).
- The system image (`$BERINGIA_ASSETS/system.apfs.img`) is a **single APFS volume** with the 25A353 OTA system tree
  and its OS-cryptex runtime (dyld shared cache, libSystem). It was assembled by hand during M5 with apfsprogs
  `mkapfs` and the experimental linux-apfs-rw write support; **no script in this repository rebuilds it yet**, and
  once macOS has mounted it read-write apfsprogs' `apfsck` no longer understands it ("Cloneinfo id epoch: not
  supported"). Keep a copy before experiments.
- launchd completes `Early boot complete. Continuing system boot.`; kernelmanagerd checks in (`KextLog:
  kernelmanagerd is active`), so deferred driver matching runs. The previous blockers and their fixes:
  - shared cache refused (`vnode is not SIP-protected`): the loader set `csrActiveConfig` without
    `kBootArgsFlagCSRActiveConfig`; now `0x6f` with the flag (the image's files lack `SF_RESTRICTED`, so
    `CSR_ALLOW_UNRESTRICTED_FS` is what is needed);
  - CarbonCore FileIDTree abort: `/System/Volumes/Preboot` is a plain directory on the root volume (hosted patch
    0039 hides it from `statfs64` with `beringia_hide_preboot=1`; a real multi-volume container is the proper fix);
  - DSMOS waiting for an SMC: `BeringiaSMC`;
  - **`gethostuuid()` blocked configd, opendirectoryd, remoted and kernelmanagerd**: the platform UUID must come from
    `/efi/platform/system-id` (the first attempt, `/options/platform-uuid`, is hidden by the NVRAM stub and never
    worked);
  - **a registry entry without a property table** (an `IOPlatformDevice` made from the NVRAM stub) panicked the kernel
    as soon as softwareupdated read the registry: hosted patch 0045.
  - **WindowServer waited forever in `IOKitWaitQuiet()`**: a serial dext matching `IOUserResources` needed
    `IOUserSerial`, which kernelmanagerd cannot load for a legacy prelinked kernel; it is in the KC now.
- **WindowServer now exits with `exit(1)` 86 ms after each launch** and launchd respawns it every 10 s. There is no
  display device (no framebuffer, no input), the expected reason; its unified log would confirm it but needs a
  tracev3 reader.
- **The image lacks most of the OS cryptex**: only 19 runtime files (dyld caches, libSystem, ...) of its 2757 file
  forks were staged. `pkd` crash-loops on `PlugInKitDaemon.framework` (not on disk, not in the dyld cache), and
  `coreduetd` (PeopleSuggester resource) probably on the same gap. The App cryptex is not staged either.
- Other crash loops: `timed` (an assertion, not analysed); `fskitd` is throttled after exit; `data-protection`
  (`seputil`, arm64e only) fails with `Bad CPU type`, which is expected on Intel.
- A self-written unsigned `/sbin/launchd` is rejected by AMFI ("The system only allows platform binaries").

## Important fixes in this round

| Problem | Root cause | Fix |
|---|---|---|
| launchd waited forever after the ignition banner | `hw.osenvironment` is released by `registerNVRAMController`, which the hosted NVRAM stub bypasses | hosted patch 0035: BeringiaPlatform calls `sysctl_unblock_osenvironment()` |
| launchd died with `EXC_BAD_ACCESS`, code `0xd 0` after ignition | Linux left **CR4.UMIP** set; macOS executes `sidt` in userspace in `__os_cpu_number`, which then raises #GP | `world_asm.S` clears CR4.UMIP on every foreign entry (commit `f661243`); since the audit, `to_linux` restores Linux's saved CR4 unconditionally |
| registry matching crashed in `ramrod`; later softwareupdated panicked the kernel in `serializeProperties` | an `IOPlatformDevice` without a property table: `createNubs` made a platform nub from the IODTNVRAM stub, whose `dictionaryWithProperties()` is NULL | patch 0038 was a per-accessor workaround; patch 0044 names such entries, patch 0045 (BeringiaPlatform does not make a nub of the NVRAM service) removes the cause |
| configd, opendirectoryd, remoted and kernelmanagerd blocked in `gethostuuid()` | x86 derives `IOPlatformUUID` from `/efi/platform/system-id`; the loader's `/options/platform-uuid` is hidden by the NVRAM stub | the loader supplies `system-id`; patch 0041 publishes the UUID |
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

- **State safety net** (`beringia_host.ko statecheck=1`, default, load-time only): after every exit from the foreign
  world, CR8, DR7, IA32_PAT, EFER, IA32_APIC_BASE, DEBUGCTL, MTRR_DEF_TYPE, MISC_ENABLE and the PMU controls are
  compared with their values at entry; differences are logged once and restored. A changed IA32_APIC_BASE, or an MSR
  that cannot be written back, ends the run. CR4 is restored by the switch itself on every exit (it clears UMIP for
  the foreign world); the net reports if the foreign kernel changed its own copy.
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
| Userland | The full system's launchd and ~130 daemons run. No interactive shell, console login or GUI; WindowServer waits in `IOKitWaitQuiet()`. |
| System image | One APFS volume (role 0), not the System/Data/Preboot/VM container macOS expects; hand-built, not reproducible from this repository yet; no `SF_RESTRICTED` flags (hence `CSR_ALLOW_UNRESTRICTED_FS`). DroidVM found single-volume and union layouts a source of daemon crash loops and moved to a real multi-volume container. |
| kexts | A legacy prelinked kernel (39 kexts). kernelmanagerd cannot load the on-disk `SystemKernelExtensions.kc` (expected with a legacy prelinked kernel). BeringiaPlatform is linked into the kernel; a standalone BeringiaCPU kext is not done. BeringiaSMC serves DSMOS only, with the DSMOS key the user supplies (`BERINGIA_OSK_FILE`). |
| trust caches | `trust_cache_init` succeeds with the img4 interface; no full-system static trust cache is supplied by the loader yet. Patch 0029 remains as the no-image4 fallback. |
| kalloc_type | Degraded to size-based data zones (upstream LLVM has no `__builtin_xnu_type_summary`). Kext views use the external fallback in the static format; no hang has been observed. |
| Block I/O | BeringiaDisk mounts APFS read-write. It reports a volatile write cache and a flush is an `fsync` of the image on the host; force-unit-access writes are not honoured individually. The read-only fallback is not validated. |
| LAPIC | A RAM page with no-op init; timer and IPIs go through the host. |
| NMI / #MC | **Not forwarded yet**: in the foreign world they enter XNU's own handlers. |
| NVRAM | IODTNVRAM is a stub; IONVRAM is announced as ready. NVRAM writes from ramrod report unsupported function. |
| SMP | One hosted CPU. The pending word is atomic on both sides already. The block host call reads its request once, and the thread dump refuses to run with more than one CPU (its walk takes no lock). |
| Display / input | None. WindowServer has no framebuffer. Note for that work: XNU runs under Linux's PAT, so its WT mappings (PA1) are write-combining and its WC mappings (PA6) are UC- (hosted patch 0025 leaves the host PAT alone). |
| Linux latency | While XNU runs with interrupts off, that Linux CPU takes no interrupts. Measure before SMP. |

## Next steps, in order

1. Make the system image reproducible from this repository (`tools/disk/`): rebuild the **whole** OS cryptex volume
   (RIDIFF forks + its APFS template) and merge it, as DroidVM does; then a real APFS container (System, Data,
   Preboot, VM) with `SF_RESTRICTED` on the system files, so the Preboot hack and `CSR_ALLOW_UNRESTRICTED_FS` can go.
   Keep a pristine copy and boot from a working copy.
2. A unified-log (tracev3) reader on Linux, to read WindowServer's and other daemons' own logs from the image.
3. A framebuffer (`BeringiaFramebuffer`: an IOFramebuffer with a linear framebuffer in the carve-out and a host
   viewer; mind the PAT note above) and input, so WindowServer finds a display; console input and a shell in
   parallel. Keep NMI/#MC forwarding, SMP and `tools/kc` CI tests as separate hardening work.

The 2026-11-27 audit of the M5 work, with every finding and what was done about it, is
[audit/2026-11-27-m5-session-audit.md](audit/2026-11-27-m5-session-audit.md).
