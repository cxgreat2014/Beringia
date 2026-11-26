#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# World-switch self-test (ROADMAP M1): run beringia_host.ko's foreign test kernel; no XNU needed. Run as root.
#   scripts/run/selftest.sh [seconds] [cpus] [spin]
# BERINGIA_NO_BUILD=1      do not build (build as your normal user; root/user builds must not be mixed)
# BERINGIA_SELFTEST_ARGS   extra beringia_host.ko parameters, e.g. 'selftest_sleep=16 selftest_tamper=1'
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"
[ "$(id -u)" = 0 ] || die "run as root (wsl -u root)"
secs=${1:-10} ncpu=${2:-2} spin=${3:-50}
[ "${BERINGIA_NO_BUILD:-0}" = 1 ] || make -s -C "$BERINGIA_ROOT/host" KDIR="$LINUX_KDIR"
sysctl -qw kernel.panic_on_oops=0
dmesg -n 7
rmmod beringia_host 2>/dev/null || true
dmesg -C
rc=0
# shellcheck disable=SC2086
insmod "$BERINGIA_ROOT/host/beringia_host.ko" selftest="$secs" selftest_cpus="$ncpu" selftest_spin="$spin" \
	${BERINGIA_SELFTEST_ARGS:-} ${BERINGIA_TRACE:+trace=$BERINGIA_TRACE} || rc=$?
dmesg | grep "beringia:"
warn=$(dmesg | grep -cE "WARNING|BUG|Oops|lockup|stall|bad value" || true)
rmmod beringia_host 2>/dev/null || true
echo "insmod rc=$rc, kernel warnings: $warn"
[ "$rc" = 0 ] && [ "$warn" = 0 ]
