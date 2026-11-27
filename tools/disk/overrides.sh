#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Bring-up changes to the macOS system image for a machine without the hardware macOS takes for granted.
#
#   sudo tools/disk/overrides.sh [--undo] [IMAGE]       IMAGE defaults to $BERINGIA_DISK or $BERINGIA_ASSETS/system.apfs.img
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
img=${1:-${BERINGIA_DISK:-$BERINGIA_ASSETS/system.apfs.img}}
[ -f "$img" ] || die "no image $img"
if ! grep -q '^apfs ' /proc/modules; then
	if [ -n "${BERINGIA_APFS_KO:-}" ]; then insmod "$BERINGIA_APFS_KO"; else modprobe apfs; fi
fi

unshare -m --propagation private bash -s "$img" "$undo" <<'EOF'
set -euo pipefail
img=$1 undo=$2
mnt=$(mktemp -d)
mount -t apfs -o readwrite,loop "$img" "$mnt"
trap 'sync; umount "$mnt"; rmdir "$mnt"' EXIT
grep -q " $mnt apfs rw" /proc/mounts || { echo "error: $img did not mount read-write" >&2; exit 1; }

disabled=$mnt/private/var/db/com.apple.xpc.launchd/disabled.plist
python3 -I - "$disabled" "$undo" <<'PY'
import os, plistlib, sys
path, undo = sys.argv[1], sys.argv[2] == "1"
d, fmt = {}, plistlib.FMT_XML
if os.path.exists(path):
    raw = open(path, "rb").read()
    fmt = plistlib.FMT_BINARY if raw.startswith(b"bplist") else plistlib.FMT_XML
    d = plistlib.loads(raw)
if undo:
    d.pop("com.apple.bluetoothd", None)
else:
    d["com.apple.bluetoothd"] = True
with open(path, "wb") as f:
    plistlib.dump(d, f, fmt=fmt)
print("disabled.plist:", sorted(k for k, v in d.items() if v))
PY

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
