<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Audit of the M4/M5 bring-up work (2026-11-26 to 2026-11-27)

The M4 and M5 work (commits `94c83f1`..`7ff73f8`, hosted patches 0023–0042, and the uncommitted loader, BeringiaSMC
and patch changes that followed) was reviewed after the fact: the host module and run scripts, the loader and kexts,
and the XNU patches, each read independently, every finding then checked against the code and, where possible, a
boot. This records what was wrong, what was fixed, and what is still open, so the same mistakes are not repeated.

## The biggest lessons

- **A fix is not done until a run shows it working.** Two fixes were reported as done that never worked: the
  platform UUID (`/options/platform-uuid` is hidden by the NVRAM stub; the same three daemons were still blocked in
  `gethostuuid()` in the next thread dump) and the UMIP restore (it depended on a debug option and made the safety
  net report our own change on every run, which also made the plain self-test fail).
- **Find the object, not the next symptom.** Three accessors in turn crashed on a registry entry with no property
  table, and each got its own NULL check (patches 0017, 0038). The fourth, `serializeProperties`, panicked the
  kernel. One hosted diagnostic (patch 0044) named the entry on the first boot: an `IOPlatformDevice` made by
  `createNubs` from the IODTNVRAM stub. Patch 0045 removes the cause.
- **Do not let an unattended run end on its own.** The watchdog's probes keep the WSL VM alive; when an armed run
  expired the probes stopped, WSL idled the VM out about a minute later and killed XNU with the 40 GiB APFS image
  mounted read-write (run `20261127-130943`). Afterwards apfsprogs could no longer check the image at all.
- **Keep assets reproducible.** The system image was assembled by hand (agent sessions, ad-hoc scripts in
  `$BERINGIA_WORK/build/tools`), has no pristine copy, and cannot be rebuilt from this repository.

## Findings and what was done

### Host module and run scripts

| Finding | Status |
|---|---|
| CR4.UMIP cleared on entry was put back only by the state safety net: every run logged "the foreign kernel changed CR4" (our own change), which used up the one CR4 report a real change would have needed; with `statecheck=0` Linux ran on with UMIP clear and a CR4 that differed from its shadow; the plain self-test failed | **Fixed**: `to_foreign` saves Linux's CR4, `to_linux` restores it unconditionally (still on the foreign CR3, PCID 0); the net now compares the foreign world's own CR4. Self-test: 0 changes without tamper |
| `statecheck` was writable at run time; switching it on mid-run compared against snapshots never taken (writing CR4 = 0, #GP) | **Fixed**: load-time only (0444) |
| A failed MSR restore was still logged as "restored" | **Fixed**: logged as failed, and it ends the run |
| The PAT tamper self-test flipped a bit every loop, so a broken restore could pass by parity | **Fixed**: one fixed tampered value |
| `stop.sh` disarmed the watchdog even when `rmmod` failed; lost `dmesg-stop.txt` for expired runs; never undid boot.sh's fail-fast settings (a later stall still reset the VM) | **Fixed**: disarms only when both modules are gone; finds an expired run through `run/last`; restores the saved settings |
| `boot.sh` left a run armed when `insmod` failed, and built the modules as root (root-owned objects in the tree) | **Fixed**: disarms on a failed load; builds as the owner of the tree |
| Watchdog expiry silently ended protection and, in practice, killed the run | **Fixed**: at expiry the watchdog runs the run's `stop.sh` |
| The watchdog treated any `vmmem` (Sandbox, Docker) as the WSL VM, and could disarm a newer run's marker | **Fixed**: `vmmemWSL` first; disarms only its own run |
| fpcheck ran on CPU 0, which the world switch never uses; memcheck `--selftest` passed despite other corruption; `run.sh` ran stale binaries after a failed build | **Fixed** (default CPU 1); fpcheck still checks only the low 128 bits of xmm0–15 (**open**) |
| `hw_check` runs after `to_linux`/`kernel_fpu_end`, so it is too late to repair some changes (EFER.NXE, CR4.PCIDE); PAT/MTRR are restored without a cache flush | **Open** (documented; none observed) |

### Loader and kexts

| Finding | Status |
|---|---|
| `/options/platform-uuid` can never be read (IODTNVRAM takes over `/options`) | **Fixed**: `/efi/platform/system-id`, verified: no `gethostuuid` waiters, kernelmanagerd checks in |
| The block request was read twice from XNU memory (validated, then re-read): another hosted CPU could change it in between | **Fixed**: copied once, only the copy is used, only `result` is written back |
| Flush was not an fsync, and BeringiaDisk reported no write cache, telling APFS its writes were durable | **Fixed**: `bh_file_sync` (vfs_fsync) in the host interface; a volatile write cache is reported |
| `pmd0=` appended after the user's boot-args: XNU's memdev scans only the first 256 bytes, so long boot-args lost the RAM-disk root; truncation was silent | **Fixed**: `pmd0` first, truncation is an error |
| No room after the RAM disk for XNU's early page tables (they can run past the carve-out into host memory) | **Fixed**: 32 MiB headroom required |
| `csrActiveConfig = 0x7f` includes `CSR_ALLOW_APPLE_INTERNAL` (internal-build behaviour, MAC panics) | **Fixed**: `0x6f` |
| BeringiaSMC: every SMC method other than `smcReadKey` dereferences AppleSMC's missing command gate; the replacement vtable was freed while the object still used it; 4096-byte vtable copy (the real one is 0x950) | **Fixed**: all 16 SMC slots replaced, static vtable checked for its exact length before init, kext refuses to unload; SMC result codes |
| `hostcalls.c` calls Linux mutex/preempt functions directly, against the "Linux only through the host interface" rule | **Open** |
| The `beringia_hc` boot argument shows a host module address to any XNU process through `kern.bootargs` | **Open** (low) |
| BeringiaSMC carried Apple's DSMOS key (the OSK0/OSK1 text) in its source | **Fixed**: the user supplies it (`BERINGIA_OSK_FILE`, [../setup/macos-assets.md](../setup/macos-assets.md)); it is in no source file, build output, log or commit |

### XNU patches

| Finding | Status |
|---|---|
| 0036 (user exception log) unguarded, printed up to four lines per exception (one per delivery level), leaked a kernel pointer | **Fixed**: hosted only, once per exception at triage, pid/name/tid |
| 0031 (address hash without corecrypto) and 0039 (hide Preboot) applied without `beringia_hosted()`; 0039 re-parsed boot-args on every `statfs64` | **Fixed** |
| 0042 walks the thread list without a lock (interrupt context) | **Fixed** for now: refuses with more than one CPU |
| `EXTERNAL_HEADERS/beringia/abi.h` in the tree had fallen behind `xnu/include/beringia/abi.h`, so a fresh prepare left a modified file and export refused to run | **Fixed**: synced (patch 0043); `export.sh` now checks |
| A registry entry without a property table (0017/0038 were per-accessor workarounds) | **Fixed**: 0044 reports such entries, 0045 removes the cause; boot `20261127-153054` reports none |
| `IODTNVRAM::copyProperty(const char *)` crashed on a NULL name (reached once the bogus nub was gone) | **Fixed** in 0032 |
| 0039 hides Preboot only from `statfs64` (`stat`, `getattrlist`, `fstatfs` still see the root device) | **Open**: the proper fix is a real Preboot volume |
| 0033 enables `CONFIG_MEMDEV_INSECURE` for the whole configuration; memdev truncates images of 4 GiB or more | **Open** (the full system uses BeringiaDisk, not memdev) |
| 0034 does not cover `IOTaskGetEntitlement`/`IOVnode*Entitlement` without AMFI | **Open** (AMFI is in the KC now) |
| 0025 leaves Linux's PAT in place: XNU's WT mappings become WC, its WC mappings UC- | **Open**: matters for the framebuffer |

### Process and documentation

| Finding | Status |
|---|---|
| The docs cited commit `403fa76` for the UMIP fix; that commit is on no branch (it was amended into `f661243`) | **Fixed** |
| `docs/current-issues.md` still described the recovery image as the stopping point; the CSR, Preboot, UUID and SMC work, BeringiaSMC and patches 0039–0042 existed only in working trees | **Fixed**: committed and documented |
| `prepare.sh`, and after edits through the Windows share also `boot.sh`, `stop.sh`, `export.sh` and `tests/host/run.sh`, lacked the executable bit | **Fixed** |
| The system image is hand-built, single-volume, not reproducible, without a pristine copy | **Open**: next step 2 in [../current-issues.md](../current-issues.md); a copy of the image as it was on 2026-11-27 is kept next to it |

## Evidence

| Run | What it shows |
|---|---|
| `20261127-130943` | The expired run that WSL killed about a minute after the watchdog disarmed it |
| `20261127-142558` | Before the fixes: configd, opendirectoryd and remoted blocked in `gethostuuid`; kernelmanagerd never checks in |
| `20261127-150801` | UUID fixed: kernelmanagerd checks in; softwareupdated then panics the kernel in `serializeProperties` |
| `20261127-151619` | Patch 0044 names the entry: `IOPlatformDevice`, registered from `IODTPlatformExpert::createNubs` |
| `20261127-153054` | Patch 0045 and the 0032 extension: no entry without a property table, no panic, ~130 daemons; WindowServer waits in `IOKitWaitQuiet()` |
