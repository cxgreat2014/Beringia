#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Create and verify an 8 MiB HFS+ RAM-disk root without mounting it.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../lib/env.sh
. "$script_dir/../lib/env.sh"
: "${BERINGIA_BUILD:=$BERINGIA_WORK/build}"

init_dir=$BERINGIA_BUILD/init
out_dir=$BERINGIA_BUILD/ramdisk
image=$out_dir/rootfs.hfs
verify_dir=$out_dir/verify
deps_dir=$BERINGIA_BUILD/deps
libdmg_src=$deps_dir/libdmg-hfsplus
libdmg_build=$deps_dir/libdmg-build
hfsplus=$libdmg_build/hfs/hfsplus
libdmg_commit=7ac55ec64c96f7800d9818ce64c79670e7f02b67

for tool in git cmake mkfs.hfsplus fsck.hfsplus; do
	command -v "$tool" >/dev/null 2>&1 ||
		die "missing $tool (install git, cmake, build-essential, zlib1g-dev, and hfsprogs)"
done
[[ -s $init_dir/launchd && -s $init_dir/hello ]] ||
	die "build the init binaries first: $BERINGIA_ROOT/tools/init/build.sh"

# Ubuntu's mkfs.hfsplus creates the volume. Its companion hfsutils only
# supports plain HFS, and this WSL kernel has no hfsplus filesystem registered.
# Use the pinned libdmg-hfsplus command-line tool for non-mount population and
# independent file extraction checks. All downloaded/build files stay in the
# external build directory, never in the Beringia repository.
if [[ ! -x $hfsplus ]]; then
	mkdir -p -- "$deps_dir"
	if [[ ! -d $libdmg_src/.git ]]; then
		rm -rf -- "$libdmg_src"
		git clone https://github.com/planetbeing/libdmg-hfsplus.git "$libdmg_src"
	fi
	if ! git -C "$libdmg_src" cat-file -e "$libdmg_commit^{commit}" 2>/dev/null; then
		git -C "$libdmg_src" fetch origin "$libdmg_commit"
	fi
	git -C "$libdmg_src" checkout --detach "$libdmg_commit"
	cmake -S "$libdmg_src" -B "$libdmg_build" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build "$libdmg_build" --target hfsplus --parallel "$BERINGIA_JOBS"
fi
[[ -x $hfsplus ]] || die "failed to build the userspace HFS+ writer"

mkdir -p -- "$out_dir" "$verify_dir"
rm -f -- "$verify_dir/launchd" "$verify_dir/hello"
tmp=$out_dir/rootfs.hfs.tmp
rm -f -- "$tmp"
truncate -s 8M "$tmp"
mkfs.hfsplus -v Beringia "$tmp"

# /dev is the devfs mount point used for /dev/console. No device nodes or
# other root files are needed for this two-program syscall test.
"$hfsplus" "$tmp" mkdir /sbin
"$hfsplus" "$tmp" mkdir /dev
"$hfsplus" "$tmp" add "$init_dir/launchd" /sbin/launchd
"$hfsplus" "$tmp" chmod 755 /sbin/launchd
"$hfsplus" "$tmp" add "$init_dir/hello" /sbin/hello
"$hfsplus" "$tmp" chmod 755 /sbin/hello
mv -f -- "$tmp" "$image"

log "listing image contents"
"$hfsplus" "$image" ls /
"$hfsplus" "$image" ls /sbin
"$hfsplus" "$image" ls /dev

log "extracting both files and comparing their bytes"
"$hfsplus" "$image" extract /sbin/launchd "$verify_dir/launchd"
"$hfsplus" "$image" extract /sbin/hello "$verify_dir/hello"
cmp -- "$init_dir/launchd" "$verify_dir/launchd"
cmp -- "$init_dir/hello" "$verify_dir/hello"
# Check after all userspace opens/closes so this validates the final image.
log "checking final HFS+ structure"
fsck.hfsplus -fn "$image"
sha256sum "$init_dir/launchd" "$verify_dir/launchd" \
	"$init_dir/hello" "$verify_dir/hello"

size=$(stat -c '%s' "$image")
printf '\nHFS+ root filesystem: %s\nSize: %s bytes (8 MiB)\n' "$image" "$size"
printf 'Verified: /sbin/launchd and /sbin/hello, mode 0755, byte-identical after extraction.\n'
