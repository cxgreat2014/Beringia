#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the x86_64 XNU kernel (DEVELOPMENT by default) from $BERINGIA_XNU_SRC on this Linux host.
#   scripts/xnu/build.sh [make target, default: build]   KERNEL_CONFIG=DEVELOPMENT|RELEASE|DEBUG
# Output: $BERINGIA_BUILD/xnu/obj/<CONFIG>_X86_64/kernel.<config>
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

: "${KERNEL_CONFIG:=DEVELOPMENT}"
target=${1:-build}
tc=$BERINGIA_TOOLCHAIN
obj=$BERINGIA_BUILD/xnu/obj
[ -x "$tc/binx86/target-clang" ] || die "no toolchain at $tc (scripts/toolchain/setup.sh)"
[ -f "$BERINGIA_XNU_MK" ] || die "no $BERINGIA_XNU_MK (scripts/toolchain/setup.sh)"
[ -d "$BERINGIA_XNU_SRC" ] || die "no XNU source (scripts/xnu/prepare.sh)"
[ "$target" = installhdrs ] || [ -f "$BERINGIA_FAKEROOT/usr/local/lib/kernel-x86_64/libfirehose_kernel.a" ] ||
	die "no fakeroot (scripts/xnu/fakeroot.sh)"

# iig outputs (DriverKit interface headers): Apple's iig compiler is not public, the generated files are arch independent.
cfg=${KERNEL_CONFIG}_X86_64
if [ -d "$BERINGIA_IIG" ]; then
	(cd "$BERINGIA_IIG" && find . -type f) | while read -r f; do
		t=$obj/${f//DEVELOPMENT_ARM64_BCM2837/$cfg}
		mkdir -p "$(dirname "$t")"
		[ -e "$t" ] || cp "$BERINGIA_IIG/$f" "$t"
		touch "$t"
	done
fi
python3 -I "$BERINGIA_ROOT/tools/build/iig_fixup.py" "$obj"

export TMPDIR=$BERINGIA_BUILD/tmp XDG_CACHE_HOME=$BERINGIA_BUILD/cache
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME"
export PATH=$BERINGIA_DROIDVM/compat/droidvm-linux/shims:$tc/binx86:$LLVM_BIN:$PATH
export PYTHONPATH=$BERINGIA_DROIDVM/compat/droidvm-linux/pyhook:$tc/python-deps
export GUNYAH_APPLE_TARGET=x86_64-apple-macos26.0 GUNYAH_LINUX_BIN=$tc/binx86 GUNYAH_LINUX=1 GUNYAH_LINUX_C_SUPPORT=0
export GUNYAH_LINUX_MAKEFILE=$BERINGIA_XNU_MK GUNYAH_LINUX_CC=$tc/binx86/target-clang GUNYAH_LINUX_CXX=$tc/binx86/target-clang++
export KERNEL_SDKROOT=$BERINGIA_FAKEROOT

cflags="-Wno-reserved-identifier -Wno-pre-c11-compat -Wno-missing-designated-field-initializers -Wno-suggest-override \
-Wno-unnecessary-virtual-specifier -Wno-suggest-destructor-override -Wno-ms-bitfield-padding ${XNU_EXTRA_CFLAGS:-}"
log "building XNU ($cfg, target $target)"
exec make -C "$BERINGIA_XNU_SRC" "$target" \
	SDKROOT=macosx ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS="$KERNEL_CONFIG" \
	KDKROOT="$BERINGIA_KDK" CFLAGS_EXTRA="$cflags" ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=x86_64 \
	DEPLOYMENT_TARGET_FLAGS="-mmacosx-version-min=26.0 -DXNU_TARGET_OS_OSX" \
	DEPLOYMENT_LINKER_FLAGS="-Wl,-macosx_version_min,26.0" \
	RC_DARWIN_KERNEL_VERSION="$XNU_DARWIN_VERSION" RC_ProjectSourceVersion="$XNU_PROJECT_VERSION" \
	BUILD_LTO=0 BOUND_CHECKS=0 BUILD_WERROR=0 MAKEJOBS=-j"$BERINGIA_JOBS" \
	LD_KERNEL_ARCHIVES="-L$BERINGIA_FAKEROOT/usr/local/lib/kernel-x86_64 -lfirehose_kernel" \
	OBJROOT="$obj" SYMROOT="$BERINGIA_BUILD/xnu/sym" DSTROOT="$BERINGIA_BUILD/xnu/dst"
