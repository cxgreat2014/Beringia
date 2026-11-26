#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build (unless BERINGIA_NO_BUILD=1), load and boot the hosted XNU. Run as root (wsl -u root).
#   scripts/run/boot.sh [seconds-to-watch]
# XNU keeps running after this script returns; scripts/run/stop.sh unloads it and disarms the watchdog.
#
# Required in local.env or the environment:
#   BERINGIA_CARVE='0x20000000:0xc0000000,0x400000000:0x100000000'   the memmap= reservations
# Optional:
#   BERINGIA_BOOT_ARGS='-v debug=0x8 ...'  XNU boot-args (default: the loader's built-in set)
#   BERINGIA_WIN_DIR=/mnt/c/Users/<you>/xnu-work
#                                  per-run directory runs/<id>/ (meta, trace, dmesg; survives a VM crash) and the
#                                  watchdog's arming marker run/armed (docs/debugging.md, "The watchdog")
#   BERINGIA_WATCH_MINUTES=60      how long the watchdog stays armed for this run
#   BERINGIA_FAILFAST=1            turn RCU stalls / soft lockups into a panic (BERINGIA_STALL_SECS, default 20), so a
#                                  wedged CPU leaves a backtrace in the host-side console log instead of a silent hang
#   BERINGIA_TRACE=PATH            the host module's trace file (default: the run directory's trace.txt)
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"
[ "$(id -u)" = 0 ] || die "run as root (wsl -u root)"
: "${BERINGIA_CARVE:?set BERINGIA_CARVE to the memmap= reservations (docs/setup/wsl2.md)}"
secs=${1:-10}

if [ "${BERINGIA_NO_BUILD:-0}" != 1 ]; then
	make -s -C "$BERINGIA_ROOT/host" KDIR="$LINUX_KDIR"
	make -s -C "$BERINGIA_ROOT/loader" KDIR="$LINUX_KDIR"
fi
img=${BERINGIA_KERNEL:-$BERINGIA_BUILD/xnu/obj/DEVELOPMENT_X86_64/kernel.development.unstripped}
[ -f "$img" ] || die "no $img (scripts/xnu/build.sh)"
mkdir -p /lib/firmware/beringia
install -m 0644 "$img" /lib/firmware/beringia/kernel

# The per-run directory on the Windows side: never reused, so one run's evidence is not overwritten by the next.
run_id=$(date +%Y%m%d-%H%M%S)
run_dir=
if [ -n "${BERINGIA_WIN_DIR:-}" ]; then
	run_dir=$BERINGIA_WIN_DIR/runs/$run_id
	mkdir -p "$run_dir" "$BERINGIA_WIN_DIR/run"
	: "${BERINGIA_TRACE:=$run_dir/trace.txt}"
	rev() { git -c safe.directory='*' -C "$1" describe --always --dirty 2>/dev/null || echo unknown; }
	{
		echo "run_id=$run_id"
		echo "date=$(date -Is)"
		echo "host_kernel=$(uname -r)"
		echo "host_cmdline=$(cat /proc/cmdline)"
		echo "beringia_rev=$(rev "$BERINGIA_ROOT")"
		echo "xnu_src=$BERINGIA_XNU_SRC"
		echo "xnu_rev=$(rev "$BERINGIA_XNU_SRC")"
		echo "kernel_sha256=$(sha256sum "$img" | cut -d' ' -f1)"
		echo "carve=$BERINGIA_CARVE"
		echo "boot_args=${BERINGIA_BOOT_ARGS:-<loader default>}"
		echo "linux_cpu=${BERINGIA_LINUX_CPU:-1}"
	} > "$run_dir/meta.txt"
else
	echo "warning: BERINGIA_WIN_DIR is not set: no run directory, and the watchdog is not armed" >&2
fi

sysctl -qw kernel.panic_on_oops=0
if [ "${BERINGIA_FAILFAST:-1}" = 1 ]; then
	sysctl -qw kernel.softlockup_panic=1 kernel.softlockup_all_cpu_backtrace=1 kernel.panic_on_rcu_stall=1
	echo "${BERINGIA_STALL_SECS:-20}" > /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout
fi
# Console log level 7: Beringia's pr_info lines also reach the console, which the host records
# (.wslconfig wsl2.debugConsoleLogFile) even when the guest is wedged.
dmesg -n 7

rmmod beringia_xnu 2>/dev/null || true
rmmod beringia_host 2>/dev/null || true
dmesg -C
[ -z "${BERINGIA_TRACE:-}" ] || : > "$BERINGIA_TRACE"

# Arm the watchdog just before the risky part (scripts/run/wsl-watchdog.ps1 reads this; stop.sh removes it).
if [ -n "$run_dir" ]; then
	arm=$BERINGIA_WIN_DIR/run/armed
	{
		echo "run_id=$run_id"
		echo "run_dir=$(wslpath -w "$run_dir")"
		echo "run_dir_wsl=$run_dir"
		echo "distro=${WSL_DISTRO_NAME:-}"
		echo "expires=$(( $(date +%s) + ${BERINGIA_WATCH_MINUTES:-60} * 60 ))"
	} > "$arm.tmp"
	mv -f "$arm.tmp" "$arm"
fi

insmod "$BERINGIA_ROOT/host/beringia_host.ko" carve="$BERINGIA_CARVE" ${BERINGIA_TRACE:+trace="$BERINGIA_TRACE"}
# Module parameters are split at spaces unless the value is in double quotes, which must reach the kernel literally.
insmod "$BERINGIA_ROOT/loader/beringia_xnu.ko" linux_cpu="${BERINGIA_LINUX_CPU:-1}" \
	${BERINGIA_BOOT_ARGS:+"boot_args=\"$BERINGIA_BOOT_ARGS\""}
sleep "$secs"
dmesg | grep -E "beringia|BUG:|Oops:|RIP:|WARNING:|rcu:|watchdog:" | tail -200
[ -z "$run_dir" ] || { dmesg > "$run_dir/dmesg.txt"; echo "run $run_id: $run_dir (watchdog armed; scripts/run/stop.sh to end)"; }
