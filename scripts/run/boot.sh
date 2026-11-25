#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Boot the hosted XNU on this machine (WSL2). Run as root.
#   scripts/run/boot.sh [extra beringia_xnu.ko parameters]
# Idempotent: reloads the modules with the current build, prints XNU's console output.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

CARVE=${BERINGIA_CARVE:-0x20000000:0xc0000000,0x400000000:0x100000000}
TRACE=${BERINGIA_TRACE:-/mnt/c/Users/solar/xnu-work/beringia-boot.txt}
# The unstripped image: strip zeroes the local-relocation table that we need to place the kernel, and we want the
# full symbol table anyway (vstart, the Beringia symbols, ...).
KERNEL=${BERINGIA_KERNEL:-$BERINGIA_BUILD/xnu/obj/DEVELOPMENT_X86_64/kernel.development.unstripped}
[ -f "$KERNEL" ] || die "no kernel at $KERNEL (scripts/xnu/build.sh)"
[ -f "$BERINGIA_ROOT/host/beringia_host.ko" ] || die "build the modules first: make -C host KDIR=\$LINUX_KDIR && make -C loader KDIR=\$LINUX_KDIR"
[ -f "$BERINGIA_ROOT/loader/beringia_xnu.ko" ] || die "build the modules first: make -C loader KDIR=\$LINUX_KDIR"
# NB: do not build here — this script runs as root, and mixing root/user builds corrupts the objects.
install -m 0644 "$KERNEL" /lib/firmware/beringia/kernel

sysctl -qw kernel.panic_on_oops=0
rmmod beringia_xnu 2>/dev/null || true
rmmod beringia_host 2>/dev/null || true
sleep 1
: > "$TRACE" 2>/dev/null || true
dmesg -C
insmod "$BERINGIA_ROOT/host/beringia_host.ko" carve="$CARVE" trace="$TRACE"
insmod "$BERINGIA_ROOT/loader/beringia_xnu.ko" "$@"
rc=$?
echo "insmod rc=$rc"
sleep "${BERINGIA_BOOT_WAIT:-8}"
dmesg | grep -E "beringia" | grep -vE "xnu: __|zone_init|submap|kmem_large|_last_kernel_symbol"
echo "=== warnings: $(dmesg | grep -cE 'WARNING|BUG|Oops|lockup|stall' || true) ==="
