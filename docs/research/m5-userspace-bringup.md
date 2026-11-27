<!-- SPDX-License-Identifier: GPL-2.0-only -->
# M5 userspace bring-up: from a RAM disk to launchd on BeringiaDisk

Date: 2026-11-27. This is an experiment log, not a claim that macOS is fully installed. It records what was changed,
which run proves it, and what is still missing.

## Result at a glance

- **M4 is working:** the legacy prelinked kernel carries the security stack and reaches the root-device wait; a
  14-minute run had no Linux warning and no state-safety-net report (`runs/20261126-224344`).
- **A real disk now works:** `BeringiaDisk` exposes a host file as an `IOBlockStorageDevice`. APFS on Apple's
  `x86_64SURamDisk.dmg` mounts from it, and the recovery launchd executes.
- **launchd gets through the whole recovery boot sequence:** after the UMIP and registry fixes, it starts
  `ramrod` and `syslogd` and performs an orderly shutdown/reboot (`runs/20261127-012220`).
- **This is not yet a shell or a full macOS installation.** `ramrod` stops with `MSU 1008 (an error occurred finding
  the target volume)` because only the recovery image is present. A complete System volume, its runtime, and an
  interactive console are still required. No GUI work has been validated.

## Evidence

| Run | What it demonstrates |
|---|---|
| `20261126-224344` | Security stack loaded; 14 minutes at "Still waiting for root device" with no host warning. |
| `20261126-233250` | APFS recovery ramdisk mounts; launchd starts but waits in `sysctl_osenvironment`. |
| `20261127-001823` | The post-ignition crash is logged as `EXC_BAD_ACCESS`, code `0xd 0`, leading to the UMIP diagnosis. |
| `20261127-011005` | First BeringiaDisk boot: boot media found, `disk0` root, APFS mount-complete, launchd starts. |
| `20261127-012220` | UMIP + `dictionaryWithProperties` fixes: launchd starts `ramrod` and `syslogd`, then shuts down because the target volume is absent. |

The run directories are under `$BERINGIA_WIN_DIR/runs/`. `meta.txt` records the exact kernel, disk image and boot
arguments for each experiment.

## 1. A root filesystem without a disk driver

The loader's `ramdisk=` parameter places an image after the boot data and adds `pmd0=<pa>.<size> rd=md0` to XNU's
command line. The image must be inside the used extent reported as `kaddr + ksize`: XNU allocates its early page
tables (`KPTphys`, from `Idle_PTs_init`) at that address. An earlier layout let those page tables overwrite the RAM
disk and then faulted in `ml_static_mfree`.

XNU's `memdev` boot arguments are enabled by hosted patch 0033. `scripts/ramdisk/build.sh` makes a small HFS+ test
root; `tools/init/` contains the static test programs.

Open-source XNU has no `bsd/hfs`. Filesystems therefore come from the KDK as kexts: `HFS.kext` for the test image,
and `apfs.kext` plus Zlib/Dataless for the recovery image. They are linked into the legacy prelinked kernel with
`tools/kc` and `scripts/kc/build.sh --closure`.

## 2. Why launchd stopped at the ignition banner

A thread dump showed launchd blocked in `_sysctl_osenvironment` on `osenvironment_initialized`. On Intel, that is
normally released by `IOPlatformExpert::registerNVRAMController`. BeringiaPlatform instead publishes `IONVRAM`
directly, because IODTNVRAM is a stub, so the release never happened.

Hosted patch 0035 calls `sysctl_unblock_osenvironment()` from `BeringiaPlatform::start`. Ignition then completes.
This was a platform-bring-up issue, not an installer error.

## 3. BeringiaDisk and a real `disk0` root

`kexts/BeringiaDisk/` implements an `IOBlockStorageDevice` backed by one raw host file. I/O is synchronous and
serialized; a physically contiguous request page and 1 MiB bounce buffer are used. The host-call ABI is:

- `BERINGIA_CALL_BLOCK_INFO` (35): block size, byte size and read-only state;
- `BERINGIA_CALL_BLOCK_RW` (34): read, write and ordered flush, with up to 16 physical segments.

The loader's `disk=PATH` parameter opens the image and services those calls. `scripts/run/boot.sh` forwards
`BERINGIA_DISK`. The kext obtains the host entry from the `beringia_hc=` boot argument, so no XNU source patch is
needed for the transport.

For the raw APFS recovery container, `rd=disk0` selects the root explicitly. The boot log shows:

```
BeringiaDisk: 524288 blocks of 512 bytes (256 MiB), writable
Got boot device = .../BeringiaDisk/IOBlockStorageDriver/Beringia Beringia Disk Media
BSD root: disk0, major 1, minor 0
disk1s1 mount-complete volume ramdisk
```

A normal installed system should use a raw whole-disk GPT image. Flush currently orders the synchronous host file
operations but is not an `fsync` to stable storage.

## 4. The post-ignition crash was `sidt` + CR4.UMIP

Hosted patch 0036 prints the exception type and Mach code words for fatal userspace exceptions. The recovery
launchd repeatedly failed with `EXC_BAD_ACCESS`, code `0xd 0`. `0xd` is the trap number (#GP), and the second word
is not a pointer address. Disassembly located the fault in `libsystem_malloc`, at a `sidt` used by
`__os_cpu_number`.

macOS deliberately executes `sidt` in userspace. Linux had left CR4.UMIP set (`0x370ef0` in the crash register
dump includes bit `0x800`), so that instruction raised #GP. It was not a NULL `memcpy` argument as the fault address
alone suggested.

Commit `f661243` clears CR4.UMIP on **every** entry to the foreign world in `world_asm.S`. (That commit left putting
Linux's UMIP back to the state safety net, which then reported our own change as "the foreign kernel changed CR4" on
every run and did nothing at all with `statecheck=0`; since the 2026-11-27 audit `to_linux` saves and restores Linux's
CR4 unconditionally, see [../audit/2026-11-27-m5-session-audit.md](../audit/2026-11-27-m5-session-audit.md).)

The same boot also needed hosted patch 0038: `dictionaryWithProperties()` now returns an empty dictionary for a nub
with no property table. Registry matching walks every service and previously dereferenced the missing table.

## 5. Current stopping point: no target System volume

With those fixes, launchd no longer dies from the userspace exception. It starts `ramrod` (PID 3) and `syslogd`
(PID 4), which is direct evidence that exec, dyld, IPC and the initial services run. `ramrod` then reports that it
cannot find a target volume and launchd reboots deliberately. That is the expected result of booting only an
installer/recovery image; it is not evidence of a complete system.

The full-system staging work extracted the 25A353 OTA, but its audit found the runtime still incomplete:
`libSystem.B.dylib`, `System/Library/dyld/dyld_shared_cache_x86_64`, OS cryptex contents and a full-system static
trust cache are missing. These must be obtained from the matching Intel runtime before a normal multi-user boot.

## Reproduce the disk boot

Build as the normal user; boot as root. Apple images and KDK content stay outside the repository.

```bash
cd ~/Beringia
. scripts/lib/env.sh
kexts/BeringiaDisk/build.sh

mapfile -t kexts < <(python3 tools/kc/deps.py "$BERINGIA_KDK_EXTENSIONS" \
  com.apple.kec.pthread com.apple.security.AppleImage4 com.apple.kext.CoreTrust \
  com.apple.driver.AppleMobileFileIntegrity com.apple.security.sandbox \
  com.apple.security.quarantine com.apple.AppleSystemPolicy \
  com.apple.driver.AppleKeyStore com.apple.kext.AppleMatch \
  com.apple.filesystems.apfs \
  com.apple.AppleFSCompression.AppleFSCompressionTypeZlib \
  com.apple.AppleFSCompression.AppleFSCompressionTypeDataless)
disk_kext=$(realpath --relative-to="$BERINGIA_KDK_EXTENSIONS" \
  "$BERINGIA_BUILD/kexts/BeringiaDisk/BeringiaDisk.kext")
scripts/kc/build.sh "${kexts[@]}" "$disk_kext"
make -C host KDIR="$LINUX_KDIR" -j"$BERINGIA_JOBS"
make -C loader KDIR="$LINUX_KDIR" -j"$BERINGIA_JOBS"
```

Then, with your own recovery image:

```bash
sudo env BERINGIA_NO_BUILD=1 \
  BERINGIA_KERNEL="$BERINGIA_BUILD/kc/prelinkedkernel" \
  BERINGIA_DISK=/absolute/path/to/x86_64SURamDisk.dmg \
  BERINGIA_BOOT_ARGS="-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1 rd=disk0" \
  scripts/run/boot.sh 90
sudo scripts/run/stop.sh
```

## Next proof points

1. Complete the Intel runtime and build the System/Data volumes; boot them through BeringiaDisk.
2. Reach a usable console/shell and record the commands run, not just launchd messages.
3. Validate block reads/writes and add real flush durability; test the read-only fallback.
4. Add console input, then networking. Display, input and WindowServer come after a stable full userland.
5. Keep NMI/#MC forwarding, SMP and CI tool tests visible as separate hardening work.
