#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Microsoft WSL2 kernel at the running version and configuration (plus KALLSYMS_ALL), so that Beringia's
# modules can be built against a tree with a matching Module.symvers and the resulting bzImage can be booted.
#   scripts/host/wsl-kernel.sh            -> $LINUX_KDIR (default $BERINGIA_WORK/src/wsl-kernel) and its bzImage
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

kdir=${LINUX_KDIR_SRC:-$BERINGIA_WORK/src/wsl-kernel}
ver=$(uname -r)
tag=linux-msft-wsl-${ver%-microsoft-standard-WSL2}
case "$ver" in *microsoft-standard-WSL2*) ;; *) die "not a WSL2 kernel ($ver)";; esac
if [ ! -d "$kdir/.git" ]; then
	log "cloning microsoft/WSL2-Linux-Kernel $tag"
	git clone -q --depth 1 --branch "$tag" https://github.com/microsoft/WSL2-Linux-Kernel.git "$kdir"
fi
cd "$kdir"
zcat /proc/config.gz > .config
scripts/config -e KALLSYMS_ALL -d DEBUG_INFO_BTF
make olddefconfig > /dev/null
log "building (this takes a while)"
make -j"$BERINGIA_JOBS" bzImage modules > "$BERINGIA_WORK/wsl-kernel-build.log" 2>&1 || die "build failed ($BERINGIA_WORK/wsl-kernel-build.log)"
log "done: $kdir/arch/x86/boot/bzImage"
echo "Copy it to Windows (e.g. C:\\Users\\<you>\\wsl-x86\\bzImage), point .wslconfig at it (docs/setup/wsl2.md),"
echo "and set LINUX_KDIR=$kdir in local.env."
