#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Suggest a kernelCommandLine for .wslconfig that reserves memory for XNU, from this VM's physical memory map.
#   scripts/host/wslconfig.sh [LOW_GiB] [HIGH_GiB]     defaults: 3 GiB below 4 GiB at 512 MiB, 4 GiB above 4 GiB
# Run as root (reads /proc/iomem). The ranges must be System RAM *before* the reservation; run it on an unreserved boot
# or check the e820 lines in dmesg.
set -euo pipefail
low=${1:-3}
high=${2:-4}
[ "$(id -u)" = 0 ] || { echo "run as root (needs /proc/iomem)"; exit 1; }
echo "# e820 RAM ranges seen by this boot:"
dmesg | grep -E 'BIOS-e820: .*usable' || true
lowbase=0x20000000
lowend=$((lowbase + (low << 30)))
[ $lowend -le $((0xf0000000)) ] || { echo "low range too large for the 32-bit hole"; exit 1; }
# the high range: 4 GiB above the 4 GiB boundary by default
highbase=0x400000000
echo
echo "kernelCommandLine=memmap=${low}G\$${lowbase} memmap=${high}G\$${highbase} nopti ibt=off nmi_watchdog=0"
echo "# then: beringia_host.ko carve=${lowbase}:$((low << 30)),${highbase}:$((high << 30))"
