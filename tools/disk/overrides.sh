#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Bring-up changes to the macOS system image for a machine without the hardware macOS takes for granted.
#
#   sudo tools/disk/overrides.sh [--undo] [IMAGE]       IMAGE defaults to $BERINGIA_DISK, else $BERINGIA_GOLDEN
#
# The golden image is chattr +i: lift that for the change (chattr -i), put it back after (chattr +i).
#
# Idempotent; --undo restores what it changed. Each change and why:
#   bluetoothd disabled (/private/var/db/com.apple.xpc.launchd/disabled.plist): there is no Bluetooth controller, so
#     bluetoothd waits forever for an IOBluetoothHCIController and never answers its clients; Language Chooser (first
#     boot) blocks in -[IOBluetoothHostController defaultController] until it does.
#   BTAudioHALPlugin.driver moved out of /System/Library/Audio/Plug-Ins/HAL (to …/HAL.beringia-disabled): it is
#     coreaudiod's client of bluetoothd's com.apple.BTAudioHALPlugin.xpc and, with bluetoothd gone, retries the lookup
#     without pause, ~7000 log entries a second, on the only hosted CPU.
# The image is mounted read-write with linux-apfs-rw ($BERINGIA_APFS_KO or an installed apfs module) in a private
# mount namespace. Not while beringia_xnu is loaded: XNU has the image open.
set -euo pipefail
. "$(dirname "$0")/../../scripts/lib/env.sh"

undo=0
if [ "${1:-}" = --undo ]; then
	undo=1
	shift
fi
[ "$(id -u)" = 0 ] || die "run as root (mount)"
grep -q '^beringia_xnu ' /proc/modules && die "beringia_xnu is loaded: end the run first (scripts/run/stop.sh)"
img=${1:-${BERINGIA_DISK:-${BERINGIA_GOLDEN:-}}}
[ -n "$img" ] && [ -f "$img" ] || die "no image ${img:-given (IMAGE, BERINGIA_DISK or BERINGIA_GOLDEN)}"
! lsattr -d "$img" 2>/dev/null | cut -d' ' -f1 | grep -q i || die "$img is immutable (chattr +i): chattr -i it first"
if ! grep -q '^apfs ' /proc/modules; then
	if [ -n "${BERINGIA_APFS_KO:-}" ]; then insmod "$BERINGIA_APFS_KO"; else modprobe apfs; fi
fi

unshare -m --propagation private bash -s "$img" "$undo" "$(cd "$(dirname "$0")" && pwd)" <<'EOF'
set -euo pipefail
img=$1 undo=$2 here=$3
mnt=$(mktemp -d)
mount -t apfs -o readwrite,loop "$img" "$mnt"
trap 'sync; umount "$mnt"; rmdir "$mnt"' EXIT
grep -q " $mnt apfs rw" /proc/mounts || { echo "error: $img did not mount read-write" >&2; exit 1; }

if [ "$undo" = 1 ]; then
	python3 -I "$here/launchd-disabled.py" --undo com.apple.bluetoothd "$mnt"
else
	python3 -I "$here/launchd-disabled.py" com.apple.bluetoothd "$mnt"
fi

hal=$mnt/System/Library/Audio/Plug-Ins/HAL
off=$mnt/System/Library/Audio/Plug-Ins/HAL.beringia-disabled
if [ "$undo" = 1 ]; then
	if [ -d "$off/BTAudioHALPlugin.driver" ]; then
		mv "$off/BTAudioHALPlugin.driver" "$hal/"
		rmdir "$off" 2>/dev/null || true
	fi
else
	mkdir -p "$off"
	[ ! -d "$hal/BTAudioHALPlugin.driver" ] || mv "$hal/BTAudioHALPlugin.driver" "$off/"
fi
echo "HAL plug-ins: $(ls "$hal" | tr '\n' ' ')"
EOF
