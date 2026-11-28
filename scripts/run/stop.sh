#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# End a hosted XNU run: save dmesg into the run directory, unload the modules, put back the host settings boot.sh
# changed, and disarm the watchdog. Run as root. The watchdog also runs it when an armed period expires.
#   scripts/run/stop.sh
# It first asks XNU to shut down orderly (module parameter shutdown; BERINGIA_SHUTDOWN=0 skips that and
# BERINGIA_SHUTDOWN_WAIT / BERINGIA_ESTOP_WAIT set the two waits, 60 s and 30 s), then unloads the modules.
# If XNU is wedged with interrupts off, rmmod waits for its CPU thread forever; recover with wsl --shutdown --force.
set -uo pipefail
. "$(dirname "$0")/../lib/env.sh"
[ "$(id -u)" = 0 ] || die "run as root (wsl -u root)"

# serialized with boot.sh (which holds the lock only while it loads the modules)
mkdir -p /run/beringia
exec 9>/run/beringia/run.lock
flock -w "${BERINGIA_LOCK_WAIT:-600}" 9 || die "another boot.sh or stop.sh holds /run/beringia/run.lock"

loaded() { grep -q -E '^beringia_(xnu|host) ' /proc/modules; }

arm=${BERINGIA_WIN_DIR:+$BERINGIA_WIN_DIR/run/armed}
run_dir=
[ -n "$arm" ] && [ -f "$arm" ] && run_dir=$(sed -n 's/^run_dir_wsl=//p' "$arm")
# An expired run is no longer armed but may still be loaded: its directory is in run/last.
if [ -z "$run_dir" ] && [ -n "${BERINGIA_WIN_DIR:-}" ] && [ -f "$BERINGIA_WIN_DIR/run/last" ] && loaded; then
	run_dir=$(cat "$BERINGIA_WIN_DIR/run/last")
fi

[ -z "$run_dir" ] || [ ! -d "$run_dir" ] || dmesg > "$run_dir/dmesg-stop.txt"

# An orderly end first: XNU shuts itself down (launchd gets SIGTERM, logd flushes, the disks are unmounted), then it
# halts and the host calls abort. If it does not answer, the kernel's own shutdown path runs; then the VM is freed
# from outside, as before. BERINGIA_SHUTDOWN=0 skips both waits (a known-wedged run).
halted() { [ "$(cat /sys/module/beringia_xnu/parameters/state 2>/dev/null)" = halted ]; }
wait_halted() {	# seconds
	local i=0
	while [ "$i" -lt "$1" ]; do
		halted && return 0
		sleep 2
		i=$((i + 2))
	done
	halted
}
# the watchdog's stop timeout is 180 s (scripts/run/wsl-watchdog.ps1): keep both waits inside it
wait1=${BERINGIA_SHUTDOWN_WAIT:-60}
wait2=${BERINGIA_ESTOP_WAIT:-30}
[ "$wait1" -le 120 ] || wait1=120
[ "$wait2" -le 60 ] || wait2=60
if loaded && [ "${BERINGIA_SHUTDOWN:-1}" != 0 ] && [ -w /sys/module/beringia_xnu/parameters/shutdown ]; then
	echo 1 > /sys/module/beringia_xnu/parameters/shutdown ||
		echo "could not ask XNU to shut down (ABI mismatch?); unloading as it is" >&2
	if wait_halted "$wait1"; then
		echo "XNU shut down and halted (launchd's path)"
	elif echo escalate > /sys/module/beringia_xnu/parameters/shutdown && wait_halted "$wait2"; then
		echo "XNU shut down and halted (the kernel's path)"
	else
		echo "XNU did not halt in time: unloading it as it is" >&2
	fi
fi
rmmod beringia_xnu 2>/dev/null
rmmod beringia_host 2>/dev/null
if loaded; then
	# Keep the run armed: XNU (or the host module) is still in the kernel, and the watchdog must keep watching it.
	die "the Beringia modules are still loaded ($(grep -o -E '^beringia_[a-z]+' /proc/modules | tr '\n' ' '))" \
		"- the run stays armed; if rmmod keeps failing, wsl --shutdown --force"
fi

# The host settings boot.sh changed (fail-fast panics, RCU stall timeout, console log level).
settings=/run/beringia/host-settings
if [ -f "$settings" ]; then
	while read -r kind key value; do
		case $kind in
		sysctl) sysctl -qw "$key=$value" ;;
		rcu_cpu_stall_timeout) echo "$key" > /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout ;;
		console_loglevel) dmesg -n "$key" ;;
		esac
	done < "$settings"
	rm -f "$settings"
fi

if [ -n "$arm" ] && [ -f "$arm" ]; then
	if [ -n "$run_dir" ] && [ -d "$run_dir" ]; then mv -f "$arm" "$run_dir/armed.stopped"; else rm -f "$arm"; fi
	echo "watchdog disarmed${run_dir:+ (run directory $run_dir)}"
fi
[ -z "${BERINGIA_WIN_DIR:-}" ] || rm -f "$BERINGIA_WIN_DIR/run/last"
echo "stopped"
