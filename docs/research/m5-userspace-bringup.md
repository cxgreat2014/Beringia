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

## 6. The full system (2026-11-27)

With the System image (`$BERINGIA_ASSETS/system.apfs.img`: the 25A353 OTA system tree plus the OS-cryptex runtime
files, one APFS volume) instead of the recovery image, the same kernel got through these, in order:

| Symptom | Cause | Fix |
|---|---|---|
| launchd: `vnode is not SIP-protected`, then `Library not loaded: /usr/lib/libobjc.A.dylib` | the loader wrote `csrActiveConfig` without `kBootArgsFlagCSRActiveConfig`, and the Linux-built volume has no `SF_RESTRICTED` flags | flag + `0x6f` |
| `init_featureflags` SIGABRT: CarbonCore `FileIDTree: volume for device id 0x1000002 already exists` | `/System/Volumes/Preboot` is a directory on the root volume, so it reports the root's device as a second volume (DroidVM hit the same) | patch 0039, `beringia_hide_preboot=1` |
| `Waiting for DSMOS...` | Apple's DSMOS needs an `AppleSMC` service | `BeringiaSMC`, with the DSMOS key you supply (`BERINGIA_OSK_FILE`, [../setup/macos-assets.md](../setup/macos-assets.md)) |
| configd, opendirectoryd, remoted, kernelmanagerd blocked in `gethostuuid()`; `registry root held busy, kernelmanagerd has not checked in` | no `IOPlatformUUID`: x86 derives it from `/efi/platform/system-id` | the loader supplies it; patch 0041 |
| kernel panic in `serializeProperties` from softwareupdated | an `IOPlatformDevice` made from the IODTNVRAM stub, with no property table | patches 0044 (names it) and 0045 (no such nub) |
| WindowServer blocked in `IOKitWaitQuiet()`: `IOKit Daemon (kernelmanagerd) stall: 'IOUserResources'` (with `beringia_busy_report=1`) | a serial dext matching `IOUserResources` needs `com.apple.driver.driverkit.serial`, which kernelmanagerd would load from the on-disk SystemKC (impossible with a legacy prelinked kernel); `kextlog=0x2f` shows `queued load request to user space` | `IOSerialFamily` + `IOUserSerial` in the prelinked kernel |

Where it stands (run `20261127-160330`): ~130 daemons, the registry goes quiet, and **WindowServer exits with
`exit(1)` 86 ms after every launch** (launchd's log: `exited due to exit(1), ran for 86ms`, respawn throttled to
10 s). There is no display device at all, which is the expected reason; its own unified log, which would say, needs
a tracev3 reader (not built yet). Other crash loops, from their `.ips` reports in `/Library/Logs/DiagnosticReports`:

- `pkd`: `Library not loaded: .../PlugInKitDaemon.framework/Versions/A/PlugInKitDaemon (no such file, not in dyld
  cache)`. The binary ships in the OS cryptex, and **only 19 runtime files of the OS cryptex payload were staged**
  (its RIDIFF payload holds 2757 file forks and an APFS template): the image lacks the rest of the cryptex.
- `coreduetd`: PeopleSuggester `+[_PSConfig _loadPlistNamed:abortOnFailure:]`, a missing resource, probably the same
  gap.
- `timed`: an assertion in timed itself; not analysed yet.

launchd's own text log (`/private/var/log/com.apple.xpc.launchd/launchd.log`) and the crash reports can be read from
Linux with the image mounted read-only (`mount -t apfs -o ro,loop`, linux-apfs-rw), even while XNU runs.

## 7. From WindowServer's exit to the first GUI pixels (2026-11-28)

Three new tools turned guessing into reading ([../debugging.md](../debugging.md)): macOS's own unified log, decoded on
Linux (`tools/debug/harvest.sh`); user-space backtraces in the thread dump, named from the dyld shared cache
(`dump=NAME`, `tools/debug/dyldsym.py`); and screenshots of a framebuffer the loader now provides
(`tools/debug/fbshot.py`). With them, each blocker named itself:

| What was seen | Where it came from | Fix |
|---|---|---|
| WindowServer `exit(1)`: `IOHIDSystem not found` / `Cannot open the HID and display/cursor system` (unified log) | no IOHIDFamily in the KC | IOHIDFamily (+ IOReportFamily, the IOHIDSystem personality) |
| `timed` SIGABRT, `TMSleepWake.m:117 rc == kIOReturnSuccess`; `coreaudiod`, `apsd`: `pm connection failed` | `IOPMConnectionCreate`: `com.apple.PowerManagement.control` unknown, **powerd never started**. Its launchd plist is a hard link to `powerd.bundle/com.apple.powerd.plist` and the image lacked it | `tools/disk/mtree.py --fix-hardlinks` against the OTA's mtree: **23 427 hard-link names** were missing (launchd plists, `CodeResources`, resources); all restored |
| WindowServer runs but is silent; main thread `usleep` in `GPUWranglerOpenWithTimeout` (CoreDisplay `CGXMappedDisplayStart`) | no GPU wrangler service | AppleGraphicsDeviceControl + AppleGPUWrangler (match `IOResources`) |
| then `sleep` in IOSurface `ioSurfaceConnectInternal`, from Metal's device enumeration | no `IOSurfaceRoot` | IOSurface.kext |
| no display at all | no framebuffer | the loader's boot framebuffer + `/display` node; IOGraphicsFamily + IONDRVSupport (`IONDRVFramebuffer` with `IOBootNDRV`) |
| WindowServer reaches `server_loop`: **the cursor is drawn** (run `20261128-021901`) | | |
| loginwindow: `usleep` loop in `main`, waiting for a flag (code peeked from memory: the binary is DSMOS-encrypted on disk) | it runs Language Chooser on a fresh system and waits for it | (expected on first boot) |
| Language Chooser: `-[IOBluetoothHostController defaultController]` waits on a semaphore | bluetoothd waits forever for an `IOBluetoothHCIController` (`Couldn't find IOBluetoothHCIController service, waiting for it to appear`); clients wait for bluetoothd | experiment: `com.apple.bluetoothd` disabled in the image's `/private/var/db/com.apple.xpc.launchd/disabled.plist` |
| then `LUICopyDefaultDesktopPicture` → StorageKit `syncAllDisks` → diskarbitrationd → FSKit `-[FSClient setupConnection]`; fskitd exits: `get client returned E0000001` / `sending mach port to kernel` | no LiveFS kernel extension | lifs.kext |
| Language Chooser idle in `-[NSApplication run]`; WindowServer has replaced the boot screen with black and the cursor (run `20261128-023734`) | | |
| coreaudiod writes 1.6 million log entries in four minutes: `Register audio plugin connection with bluetoothd` / `Connection invalid` | its BTAudioHALPlugin retries without pause once bluetoothd is disabled | `tools/disk/overrides.sh` also moves the plug-in aside |
| black screen: CoreDisplay `IOFBSetDisplayModeAndDepth: Setting mode 0x0` / `Failed to obtain mode info`, `Setting main display ... offline in CGXConstructVirtualFramebuffer`: WindowServer composites into a virtual framebuffer | CoreDisplay's port of IOGraphicsLib (disassembled from the shared cache) calls a connection usable only if an `IODisplay` already sits under the framebuffer when it looks; the display attaches only once the framebuffer is opened, i.e. after WindowServer looked. IOBootNDRV also describes its one mode poorly (refresh 0, no timing, no EDID) | `BeringiaFramebuffer` (an IOFramebuffer with a 60 Hz detailed timing and an EDID) that opens itself at boot and raises the connect-change interrupt when the display attaches: CoreDisplay then sets mode 1, `Mode has dimensions 1280x800` (run `20261128-035849`) |
| the screen is still black, with the cursor; WindowServer orders Language Chooser's window front | run `20261128-042946`: `Installer Progress` releases its CG shield window 20 s in, SkyLight starts its `Software compositor`, and IOPresentment logs `Unable to acquire map` while CoreDisplay builds the pipe for our framebuffer. That "software compositor" renders on the CPU but still scans out through the CoreDisplay DisplayPipe, whose presentation is IOPresentment: it needs an AppleGPUWrangler GPU with an `IOAccelerator` under an `IOPCIDevice`. Our framebuffer has none of that (GPUWrangler: `(NG) ... pci 0 ... accel 0`), so a bare IOFramebuffer cannot receive pixels. Apple's own software path is a fake PCI GPU: `AppleParavirtGPU` (IOPCIMatch `0xEEEE106B`, AcceleratorProperties with `IOAccelDisplayPipeCapabilities`) | next: a virtual `IOPCIDevice` with a minimal `IOAccelerator` (BeringiaPCI), or AppleParavirtGPU's shape with our framebuffer as its aperture |

The kernel collection is now 50 kexts, `EndpointSecurity` among them for `endpointsecurityd`'s loop
([scripts/kc/full-system.kexts](../../scripts/kc/full-system.kexts) says why
each is there, plus BeringiaDisk, BeringiaSMC and BeringiaFramebuffer).

macOS's logd keeps recent entries in memory, so `stop.sh` now ends a run with a real shutdown (patch 0048), and logd
flushes on the way. Crash loops left: `pkd` and `mobileassetd` (both from the missing OS cryptex content),
`Kerberos.digest-service` and the `distnoted` agent (`exit(78)`) were historical, not seen in recent runs; `timed`,
`coreduetd`, `endpointsecurityd` (run `20261128-051403`) and `cryptexd` (patch 0049: `codex_init` stats
`/System/Volumes/Preboot`, which the single-volume image's hide made return ENOENT; run `20261128-054029`) no longer
crash.

## Reproduce the full-system boot

The KDK part of the kernel collection is [scripts/kc/full-system.kexts](../../scripts/kc/full-system.kexts); the three
Beringia kexts (BeringiaDisk, BeringiaSMC, BeringiaFramebuffer) go after it. `BERINGIA_OSK_FILE` (in `local.env`) must name your DSMOS key file
([../setup/macos-assets.md](../setup/macos-assets.md)), or DSMOS keeps waiting:

```bash
cd ~/Beringia && . scripts/lib/env.sh
kexts/BeringiaDisk/build.sh && kexts/BeringiaSMC/build.sh && kexts/BeringiaFramebuffer/build.sh
mapfile -t kexts < <(grep -v '^#' scripts/kc/full-system.kexts)
for k in BeringiaDisk BeringiaSMC BeringiaFramebuffer; do
  kexts+=("$(realpath --relative-to="$BERINGIA_KDK_EXTENSIONS" "$BERINGIA_BUILD/kexts/$k/$k.kext")")
done
scripts/kc/build.sh "${kexts[@]}"
# local.env: BERINGIA_GOLDEN (the image, chattr +i), BERINGIA_KERNEL=/home/<you>/beringia-work/build/kc/prelinkedkernel,
# BERINGIA_BOOT_ARGS="-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1 rd=disk0 beringia_hide_preboot=1 beringia_no_coredump=1"
sudo scripts/run/experiment.sh --note "full system" --shots 60,120,180 --dump WindowServer
```

Add `beringia_busy_report=1` to see what holds the registry busy, `kextlog=0x2f` for kext load requests. macOS
mounts the image read-write, so every run boots its own copy of the golden image ([../debugging.md](../debugging.md),
"Experiments"); `boot.sh` with `BERINGIA_GOLDEN` set does the same for an interactive run.

Two changes to the image are part of the 2026-11-28 state (section 7), until the image builder does them: the lost
hard links restored with `tools/disk/mtree.py check MTREE ROOT --fix-hardlinks` (on the image mounted read-write, or on
the staging tree before the image is built), and `sudo tools/disk/overrides.sh` (bluetoothd disabled, coreaudiod's
Bluetooth audio plug-in moved aside; `--undo` reverts). The loader's framebuffer is on by default (`BERINGIA_FB`);
`BeringiaFramebuffer` drives it, IONDRVSupport's `IONDRVFramebuffer` takes over if that kext is not in the KC.

## Next proof points

1. Complete the Intel runtime and build the System/Data volumes; boot them through BeringiaDisk.
2. Reach a usable console/shell and record the commands run, not just launchd messages.
3. Validate block reads/writes and add real flush durability; test the read-only fallback.
4. Add console input, then networking. Display, input and WindowServer come after a stable full userland.
5. Keep NMI/#MC forwarding, SMP and CI tool tests visible as separate hardening work.
