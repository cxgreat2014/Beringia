<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Debugging Beringia

How to boot the hosted XNU, read its console and panics, symbolize a backtrace, and recover a wedged WSL VM.

## Booting

```sh
# as root, in WSL:
bash scripts/run/boot.sh [extra beringia_xnu.ko parameters]
```

`boot.sh` rebuilds nothing (build the modules as your normal user first — see below), installs the unstripped kernel
to `/lib/firmware/beringia/kernel`, loads `beringia_host.ko` with the carve-outs, then `beringia_xnu.ko`, and prints
XNU's console. It sets `kernel.panic_on_oops=0` so an XNU bug does not reboot the VM.

```sh
# build the modules (as your normal user, NOT root — mixing root/user builds corrupts the objects):
make -C host    KDIR=$LINUX_KDIR
make -C loader  KDIR=$LINUX_KDIR
# then boot as root:
bash scripts/run/boot.sh
```

`LINUX_KDIR` is the WSL2 kernel tree you built (`scripts/host/wsl-kernel.sh`). Paths are in `local.env` (git-ignored;
absolute paths so they also work under sudo/root).

## Where the console output goes

- **`dmesg`** — every XNU console line and panic, prefixed `beringia:`. Read with `dmesg | grep beringia`.
- **A Windows-side trace file** (`/mnt/c/Users/solar/xnu-work/beringia-boot.txt` by default, `beringia_host.ko trace=`).
  Lines are flushed by an *unbound* workqueue so they survive even when the CPU that made the host call is stuck in
  the foreign kernel or a panic. **This is the file that survives a hard VM crash.**

`boot.sh` truncates the trace on each run. `BERINGIA_TRACE` overrides the path.

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

## Useful module parameters

| Parameter | Effect |
|---|---|
| `beringia_host.ko carve=BASE:SIZE[,BASE:SIZE...]` | physical RAM for XNU (reserved with `memmap=`). |
| `beringia_host.ko trace=PATH` | also append log lines to this file. |
| `beringia_host.ko selftest=N selftest_cpus=M` | run the world-switch self-test (no XNU needed). |
| `beringia_xnu.ko kernel=PATH` | the kernel Mach-O to load (default `/lib/firmware/beringia/kernel`). |
| `beringia_xnu.ko boot_args="..."` | XNU boot arguments. Default includes `-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1`. |
| `beringia_xnu.ko linux_cpu=N` | which Linux CPU runs hosted CPU 0 (default 1). |

## Recovering a wedged WSL VM

If WSL stops responding (an XNU or world-switch bug can wedge the guest):

```sh
wsl.exe --shutdown --force     # plain --shutdown can block forever on a stuck instance
```

There is also `scripts/run/wsl-recover.sh`. **A watchdog** (`scripts/run/wsl-watchdog.ps1`) automates this for you —
see below.

After recovering, `bash scripts/run/boot.sh` again (the trace file on the Windows side has the pre-crash console).

## The watchdog

`scripts/run/wsl-watchdog.ps1` (Windows PowerShell) watches the newest Claude Code session transcript and runs
`wsl --shutdown --force` when it has been inactive for 10 minutes — exactly the situation where a WSL kernel bug
hangs the agent mid-command.

```powershell
# register (once, at logon) and start now:
powershell -File scripts/run/install-watchdog.ps1
```

`install-watchdog.ps1` registers the `BeringiaWSLWatchdog` scheduled task and starts it. The watchdog logs to
`%USERPROFILE%\xnu-work\beringia-watchdog.log`. It is a blunt instrument: it kills all WSL instances on inactivity,
so unsaved WSL work is lost by design — use it while debugging the kernel, not as a general utility.
