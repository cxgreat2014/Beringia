#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# End a hosted XNU run: save dmesg into the run directory, unload the modules, disarm the watchdog. Run as root.
#   scripts/run/stop.sh
# If XNU is wedged with interrupts off, rmmod waits for its CPU thread forever; recover with wsl --shutdown --force.
set -uo pipefail
. "$(dirname "$0")/../lib/env.sh"
[ "$(id -u)" = 0 ] || die "run as root (wsl -u root)"

arm=${BERINGIA_WIN_DIR:+$BERINGIA_WIN_DIR/run/armed}
run_dir=
[ -n "$arm" ] && [ -f "$arm" ] && run_dir=$(sed -n 's/^run_dir_wsl=//p' "$arm")

[ -z "$run_dir" ] || dmesg > "$run_dir/dmesg-stop.txt"
rmmod beringia_xnu 2>/dev/null || true
rmmod beringia_host 2>/dev/null || true
if [ -n "$arm" ] && [ -f "$arm" ]; then
	if [ -n "$run_dir" ] && [ -d "$run_dir" ]; then mv -f "$arm" "$run_dir/armed.stopped"; else rm -f "$arm"; fi
	echo "watchdog disarmed${run_dir:+ (run directory $run_dir)}"
fi
