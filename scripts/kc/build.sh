#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build a prelinked kernel: the built XNU kernel plus kexts from your KDK, linked the way OpenCore injects kexts.
#
#   scripts/kc/build.sh [KEXT ...]              default: pthread
#   scripts/kc/build.sh --closure BUNDLE_ID...  the dependency closure of these kexts (tools/kc/deps.py)
#
# KEXT is a bundle name in $BERINGIA_KDK_EXTENSIONS (pthread, corecrypto), or a bundle path relative to it
# (AppleStorageDrivers.kext/Contents/PlugIns/AppleUSBTDM.kext), in load order. The result is
# $BERINGIA_BUILD/kc/prelinkedkernel; boot it with BERINGIA_KERNEL=$BERINGIA_BUILD/kc/prelinkedkernel scripts/run/boot.sh.
# Fetches OpenCorePkg and acidanthera's EDK2 (audk, headers only) at pinned revisions into $BERINGIA_WORK/deps.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

: "${BERINGIA_KC_KERNEL:=$BERINGIA_BUILD/xnu/obj/DEVELOPMENT_X86_64/kernel.development.unstripped}"
out=$BERINGIA_BUILD/kc
mkdir -p "$out"
if [ "${1:-}" = --closure ]; then
	shift
	[ $# -gt 0 ] || die "--closure needs at least one bundle identifier"
	python3 "$BERINGIA_ROOT/tools/kc/deps.py" "$BERINGIA_KDK_EXTENSIONS" "$@" | tee "$out/kexts.txt"
	mapfile -t kexts < "$out/kexts.txt"
	set -- "${kexts[@]}"
fi
[ $# -gt 0 ] || set -- pthread

fetch() {	# dir url commit
	if [ ! -d "$1/.git" ]; then
		log "fetching $(basename "$1") $3"
		git init -q "$1"
		git -C "$1" remote add origin "$2"
	fi
	if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
		git -C "$1" fetch -q --depth 1 origin "$3"
		git -C "$1" -c advice.detachedHead=false checkout -q "$3"
	fi
}
fetch "$BERINGIA_OPENCORE" "$OPENCORE_GIT" "$OPENCORE_COMMIT"
fetch "$BERINGIA_AUDK" "$AUDK_GIT" "$AUDK_COMMIT"

mkdir -p "$out/thin"
log "building kcinject"
make -s -C "$out" -f "$BERINGIA_ROOT/tools/kc/Makefile" -j"$BERINGIA_JOBS" \
	OC_USER="$BERINGIA_OPENCORE" UDK_PATH="$BERINGIA_AUDK" WERROR=1 >/dev/null

[ -f "$BERINGIA_KC_KERNEL" ] || die "no kernel at $BERINGIA_KC_KERNEL (scripts/xnu/build.sh)"
[ -d "$BERINGIA_KDK_EXTENSIONS/System.kext/PlugIns" ] || die "no KDK extensions at $BERINGIA_KDK_EXTENSIONS"
python3 "$BERINGIA_ROOT/tools/kc/skeleton.py" "$BERINGIA_KC_KERNEL" "$BERINGIA_KDK_EXTENSIONS/System.kext/PlugIns" \
	"$out/skeleton"

args=()
for k in "$@"; do
	case $k in
	*.kext) b=$BERINGIA_KDK_EXTENSIONS/$k ;;
	*) b=$BERINGIA_KDK_EXTENSIONS/$k.kext ;;
	esac
	rel=${b#"$BERINGIA_KDK_EXTENSIONS"/}
	name=$(basename "$b" .kext)
	[ -f "$b/Contents/Info.plist" ] || die "no $b/Contents/Info.plist"
	exe=$(python3 -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb")).get("CFBundleExecutable", ""))' \
		"$b/Contents/Info.plist")
	if [ -n "$exe" ] && [ -f "$b/Contents/MacOS/$exe" ]; then
		archs=$("$LLVM_BIN/llvm-lipo" "$b/Contents/MacOS/$exe" -archs)
		if [ "${archs// /}" != x86_64 ]; then
			"$LLVM_BIN/llvm-lipo" "$b/Contents/MacOS/$exe" -thin x86_64 -output "$out/thin/$name"
		else
			cp "$b/Contents/MacOS/$exe" "$out/thin/$name"
		fi
		python3 "$BERINGIA_ROOT/tools/kc/kpibind.py" "$out/thin/$name" "$b/Contents/Info.plist" "$BERINGIA_KDK_EXTENSIONS" \
			"$out/thin/$name.bound"
		args+=("/System/Library/Extensions/$rel" "Contents/MacOS/$exe" "$out/thin/$name.bound" "$b/Contents/Info.plist")
	else
		args+=("/System/Library/Extensions/$rel" - - "$b/Contents/Info.plist")
	fi
done
printf '%s\n' "$@" > "$out/kexts.txt"
"$out/kcinject" "$out/skeleton" "$out/prelinkedkernel" "${args[@]}"
