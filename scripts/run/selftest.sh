#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# World-switch self-test (ROADMAP M1): build beringia_host.ko and run its foreign test kernel; no XNU needed.
#   sudo scripts/run/selftest.sh [seconds] [cpus] [spin]
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"
secs=${1:-10} ncpu=${2:-2} spin=${3:-50}
make -s -C "$BERINGIA_ROOT/host" KDIR="$LINUX_KDIR"
sysctl -qw kernel.panic_on_oops=0
rmmod beringia_host 2>/dev/null || true
dmesg -C
insmod "$BERINGIA_ROOT/host/beringia_host.ko" selftest="$secs" selftest_cpus="$ncpu" selftest_spin="$spin" ${BERINGIA_TRACE:+trace=$BERINGIA_TRACE}
rc=$?
dmesg | grep "beringia:"
warn=$(dmesg | grep -cE "WARNING|BUG|Oops|lockup|stall|bad value" || true)
rmmod beringia_host 2>/dev/null || true
echo "insmod rc=$rc, kernel warnings: $warn"
[ "$rc" = 0 ] && [ "$warn" = 0 ]
