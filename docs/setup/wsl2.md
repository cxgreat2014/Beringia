<!-- SPDX-License-Identifier: GPL-2.0-only -->
# WSL2 host setup

Beringia's first host is WSL2 on an Intel x86_64 Windows PC. You need a custom WSL2 kernel (to build modules against
it with matching symbol versions) and memory reserved for XNU.

## 1. Kernel

Build the Microsoft WSL2 kernel at the running version with the running configuration:

```sh
scripts/host/wsl-kernel.sh          # clones microsoft/WSL2-Linux-Kernel at linux-msft-wsl-<version>, builds bzImage
```

It enables `KALLSYMS_ALL` and keeps everything else as shipped. Copy `arch/x86/boot/bzImage` to Windows and point
`.wslconfig` at it (below). The modules are then built against that tree (`LINUX_KDIR`).

## 2. `.wslconfig`

`%USERPROFILE%\.wslconfig` (example for a 32 GiB PC, 20 CPUs):

```ini
[wsl2]
kernel=C:\\Users\\<you>\\wsl-x86\\bzImage
memory=24GB
processors=20
kernelCommandLine=memmap=3G$0x20000000 memmap=4G$0x400000000 nopti ibt=off nmi_watchdog=0
debugConsole=true
debugConsoleLogFile=C:\\Users\\<you>\\xnu-work\\logs\\wsl-console.log
```

- `memmap=SIZE$BASE` hides physical RAM from Linux; Beringia gives it to XNU. The first range must be below 4 GiB
  and should start at 512 MiB (`0x20000000`): XNU's kernel is placed at its base. Check the guest's physical map
  first (`sudo cat /proc/iomem`): both ranges must lie inside what was "System RAM" before.
  `scripts/host/wslconfig.sh` prints a suitable line for the current VM.
- `nopti`: no separate user page tables in Linux, so Linux's entry code never rewrites CR3 under XNU.
  (Security trade-off on CPUs vulnerable to Meltdown; acceptable for a development VM.)
- `ibt=off`: no indirect-branch tracking in Linux's kernel; XNU's code has no ENDBR landing pads.
- `nmi_watchdog=0`: no perf NMIs while XNU runs.
- `debugConsole=true`: a Windows console window with the guest's kernel log, which survives a guest crash.
- `debugConsoleLogFile=...`: the same console, written to a file by Windows (WSL 2.x/3.x; the directory must exist).
  It is the record that survives a wedged or reset guest, and it is appended across VM restarts.

Restart WSL (`wsl --shutdown`) and check:

```sh
cat /proc/cmdline                  # the options above
sudo grep -v '^ ' /proc/iomem      # the reserved ranges are no longer System RAM
```

## 3. Crash handling

- `sudo sysctl -w kernel.panic_on_oops=0`: WSL sets `panic_on_oops=1` and `panic=-1`, which turns any bug into an
  instant VM reset (`boot.sh` does this).
- `scripts/run/boot.sh` turns RCU stalls and soft lockups into panics (`BERINGIA_FAILFAST`), so a wedged CPU produces
  backtraces in the host-side console log and a VM reset instead of a silent hang.
- `scripts/run/wsl-watchdog.ps1` (installed with `scripts/run/install-watchdog.ps1`) watches armed runs, keeps their
  evidence and force-stops a hung VM; see [../debugging.md](../debugging.md).
- If the VM stops responding: `wsl --shutdown --force` (plain `--shutdown` waits for the instance and can block
  forever). `scripts/run/wsl-recover.sh` does this from inside a working shell.
