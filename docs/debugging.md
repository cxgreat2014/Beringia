<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Debugging Beringia

How to boot the hosted XNU, read its console and panics, symbolize a backtrace, and recover a wedged WSL VM.

## Booting

```sh
# build the modules as your normal user (NOT root: mixing root/user builds corrupts the objects):
make -C host    KDIR=$LINUX_KDIR
make -C loader  KDIR=$LINUX_KDIR
# boot as root (wsl -u root), watch the console for N seconds (default 10):
BERINGIA_NO_BUILD=1 bash scripts/run/boot.sh [N]
# end the run: save dmesg, unload, disarm the watchdog
bash scripts/run/stop.sh
```

`boot.sh` installs the unstripped kernel to `/lib/firmware/beringia/kernel`, creates a run directory (below), arms the
watchdog, loads `beringia_host.ko` with the carve-outs, then `beringia_xnu.ko`, and prints XNU's console. XNU keeps
running after the script returns. Settings (in `local.env` or the environment; see the header of `boot.sh`):

| Variable | Effect |
|---|---|
| `BERINGIA_BOOT_ARGS` | XNU boot-args. Passed quoted (`boot_args="..."`), so the kernel does not split them at spaces. |
| `BERINGIA_KERNEL` | kernel/prelinked-kernel image to install and boot (default: the XNU build output). |
| `BERINGIA_RAMDISK` | optional HFS+/APFS image loaded as memdev `pmd0`, with `rd=md0` appended. |
| `BERINGIA_DISK` | optional raw host file exposed through BeringiaDisk as block unit 0. For the recovery container, add `rd=disk0` to `BERINGIA_BOOT_ARGS`. |
| `BERINGIA_WIN_DIR` | e.g. `/mnt/c/Users/<you>/xnu-work`: per-run directories and the watchdog's marker live here. |
| `BERINGIA_WATCH_MINUTES` | how long the watchdog stays armed for the run (default 60). |
| `BERINGIA_FAILFAST` | default 1: `softlockup_panic`, `panic_on_rcu_stall` and an RCU stall timeout of `BERINGIA_STALL_SECS` (20). A wedged CPU then ends in a panic with backtraces on the console instead of a silent hang. |
| `BERINGIA_LINUX_CPU` | the Linux CPU that runs hosted CPU 0 (default 1). |

It also sets `kernel.panic_on_oops=0` (an oops in a module thread does not reset the VM) and the console log level to
7, so Beringia's `pr_info` lines reach the console and therefore the host-side console log.

`LINUX_KDIR` is the WSL2 kernel tree you built (`scripts/host/wsl-kernel.sh`). Paths are in `local.env` (git-ignored;
absolute paths so they also work under sudo/root).

A BeringiaDisk recovery boot looks like this (use your own image; do not add `ramdisk=` at the same time):

```sh
sudo env BERINGIA_NO_BUILD=1 \
  BERINGIA_KERNEL="$BERINGIA_BUILD/kc/prelinkedkernel" \
  BERINGIA_DISK=/absolute/path/to/x86_64SURamDisk.dmg \
  BERINGIA_BOOT_ARGS="-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1 rd=disk0" \
  scripts/run/boot.sh 90
sudo scripts/run/stop.sh
```

Expected evidence includes `BeringiaDisk: ... blocks of 512 bytes`, `Got boot device`, `BSD root: disk0`, and an
APFS `mount-complete` line. The M5 experiment log and KC build commands are in
[research/m5-userspace-bringup.md](research/m5-userspace-bringup.md).

## Where the console output goes

- **The host-side console log** (`.wslconfig`: `debugConsoleLogFile`, see [setup/wsl2.md](setup/wsl2.md)). Written by
  Windows from the guest's `hvc0` console, so it keeps everything up to the last instruction of a wedged or panicking
  guest, including the RCU-stall and soft-lockup backtraces. It is appended across VM restarts. **This is the primary
  crash record.** The watchdog copies it into the run directory while a run is armed.
- **The run directory** `$BERINGIA_WIN_DIR/runs/<YYYYmmdd-HHMMSS>/`: `meta.txt` (revisions of both trees, kernel
  SHA-256, boot-args, host command line), `trace.txt` (the host module's trace file), `dmesg.txt` / `dmesg-stop.txt`,
  `console.log` (mirror) and, after an incident, `incident-*.txt`, `console-*.log`, `watchdog-*.log`. A run directory is
  never reused, so the next run cannot overwrite the evidence of the previous one.
- **`dmesg`**: every XNU console line and panic, prefixed `beringia:`. Lost with the VM.
- **The module trace file** (`beringia_host.ko trace=`, by default the run directory's `trace.txt`): flushed by an
  unbound workqueue through drvfs. It needs a working Linux to run that workqueue, so on a whole-VM hang its last
  lines are usually missing; prefer the console log.

The host interface for logging is `bh_log`/`bh_logf` (kernel log + trace ring) and `bh_console` (a console line from a
host call). Console output from the foreign kernel reaches the host through the `CONSOLE`/`PANIC` hostcalls.

## Reading a panic

XNU prints a full register dump and backtrace on its console (they reach `dmesg` and the trace file). The run then
ends cleanly (`halt_all_cpus` is patched to end the hosted run rather than reset the VM).

To symbolize a backtrace address: XNU runs **slid** (by the low carve-out base, usually `0x20000000`). Subtract the
slide, then look up the symbol in the unstripped kernel:

```sh
NM=~/w/dvm/toolchains/linux-x86/toolchain/bin/x86_64-apple-darwin-nm
K=~/beringia-work/build/xnu/obj/DEVELOPMENT_X86_64/kernel.development.unstripped
$NM $K | grep ' [Tt] ' | sort > /tmp/syms.txt
python3 - <<'EOF'
import bisect
syms=[]
for l in open('/tmp/syms.txt'):
    p=l.split()
    if len(p)>=3:
        try: syms.append((int(p[0],16),p[2]))
        except: pass
syms.sort(); addrs=[s[0] for s in syms]
for r in [0xffffff8020c77601]:           # a backtrace return address
    u=r-0x20000000                        # subtract the slide
    i=bisect.bisect_right(addrs,u)-1
    print(hex(r), syms[i][1] if i>=0 else None)
EOF
```

Disassemble at an address with `llvm-objdump -d --start-address=0x... --stop-address=0x... $K` (same slide rule).
**Note that adding debug code shifts symbol addresses** — re-run `nm` after each build rather than reusing an old map.

## Userspace exceptions

Hosted patch 0036 prints `BERINGIA: user exception ...` for `EXC_BAD_ACCESS`, `EXC_BAD_INSTRUCTION` and
`EXC_ARITHMETIC`, including the two Mach code words. For example, launchd's post-ignition failure showed code
`0xd 0`: `0xd` is #GP from userspace `sidt` with CR4.UMIP set, not a NULL-pointer memcpy. The world switch now
clears UMIP on every foreign entry; Linux's CR4 is restored on exit.

For a userspace crash, also record the RIP, the `uuid info` image map, and the exception code before disassembling
the corresponding library. See [research/m5-userspace-bringup.md](research/m5-userspace-bringup.md) for the worked
example.

## Where is XNU stuck? The thread dump

When XNU stops making progress without a panic (it idles with a far deadline), ask it for its threads:

```sh
echo 1 > /sys/module/beringia_xnu/parameters/dump        # as root, while beringia_xnu.ko is loaded
dmesg | grep 'BERINGIA dump' | sed 's/^.*BERINGIA dump: //' |
	python3 tools/debug/symbolize.py ~/beringia-work/build/xnu/obj/DEVELOPMENT_X86_64/kernel.development.unstripped
```

Every thread is printed with its name, state, priority, wait event, continuation and a frame-pointer backtrace;
`symbolize.py` annotates every slid kernel address (`0xffffff80...`) with `symbol+offset`. Wait events are usually the
address of the object waited on, so they symbolize too (e.g. `_max_cpus_initialized`). The same script works on panic
backtraces.

## Useful module parameters

| Parameter | Effect |
|---|---|
| `beringia_host.ko statecheck=1` | (default) the state safety net: Linux's unswitched CPU state is compared after every foreign-world exit, reported and restored. |
| `beringia_host.ko selftest_sleep=N selftest_tamper=1` | self-test: every Nth host call sleeps in `bh_idle`; the test kernel changes IA32_PAT behind the host's back. |
| `beringia_xnu.ko dump` (write) | the thread dump above. |
| `beringia_host.ko carve=BASE:SIZE[,BASE:SIZE...]` | physical RAM for XNU (reserved with `memmap=`). |
| `beringia_host.ko trace=PATH` | also append log lines to this file. |
| `beringia_host.ko selftest=N selftest_cpus=M` | run the world-switch self-test (no XNU needed). |
| `beringia_xnu.ko kernel=PATH` | the kernel Mach-O to load (default `/lib/firmware/beringia/kernel`). |
| `beringia_xnu.ko ramdisk=PATH` | a memory-disk image for memdev (`pmd0` + `rd=md0`). |
| `beringia_xnu.ko disk=PATH` | a raw host file used by BeringiaDisk (block unit 0); opened read/write when possible, otherwise read-only. |
| `beringia_xnu.ko boot_args="..."` | XNU boot arguments. Default includes `-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1`. |
| `beringia_xnu.ko linux_cpu=N` | which Linux CPU runs hosted CPU 0 (default 1). |

## Recovering a wedged WSL VM

If WSL stops responding (an XNU or world-switch bug can wedge the guest):

```sh
wsl.exe --shutdown --force     # plain --shutdown can block forever on a stuck instance
```

There is also `scripts/run/wsl-recover.sh`. During an armed run the watchdog does this for you, after saving the
evidence (below). Afterwards, read the run directory (the console log first), then boot again.

## The watchdog

`scripts/run/wsl-watchdog.ps1` (Windows PowerShell, a scheduled task) protects **armed runs** only:

- `boot.sh` arms it by writing `$BERINGIA_WIN_DIR/run/armed` (run id, run directory, distro, expiry) just before it
  loads the modules; `stop.sh` disarms it; an armed run expires after `BERINGIA_WATCH_MINUTES`.
- While nothing is armed it does nothing at all: it never runs `wsl.exe`, so it neither keeps the VM alive nor starts
  it again after a crash.
- While a run is armed it polls every 10 s: it mirrors the host-side console log into the run directory, checks that
  the WSL VM is alive (the `vmmemWSL` process, with the same id as when the run started) and that the distro answers
  `wsl -e true` within 20 s. It checks the VM rather than `wsl --list --running` on purpose: WSL stops an idle
  distro after a few seconds while the VM, and XNU in its kernel, keep running.
- **vm-exited** (VM process gone or replaced, typically a panic with `panic=-1`): evidence saved, run disarmed.
- **hang** (no answer three polls in a row, about a minute and a half): evidence saved, then
  `wsl --shutdown --force` (this kills all WSL distros), run disarmed.
- Evidence: `incident-*.txt`, `console-*.log` and `watchdog-*.log` in the run directory. The watchdog's own log is
  `%USERPROFILE%\xnu-work\beringia-watchdog.log` (state changes and incidents only).

```powershell
# install or update (stops the old instance, copies the script to %USERPROFILE%\xnu-work, starts it at logon):
powershell -ExecutionPolicy Bypass -File scripts\run\install-watchdog.ps1
# try it without consequences: one poll / simulated hang, never shuts WSL down
powershell -File wsl-watchdog.ps1 -DryRun -ProbeCommand 'sleep 60' -ProbeTimeoutSeconds 3 -PollSeconds 2
```

Run the installer from a Windows path (copy it out of `\\wsl.localhost\...` first if PowerShell refuses the UNC path).
