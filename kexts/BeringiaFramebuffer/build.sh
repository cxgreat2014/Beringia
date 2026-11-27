#!/usr/bin/env bash
# SPDX-License-Identifier: APSL-2.0
# Build BeringiaFramebuffer.kext as a thin x86_64 MH_KEXT_BUNDLE for scripts/kc/build.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=../../scripts/lib/env.sh
. "$here/../../scripts/lib/env.sh"

out=$BERINGIA_BUILD/kexts/BeringiaFramebuffer
bundle=$out/BeringiaFramebuffer.kext
exe=$bundle/Contents/MacOS/BeringiaFramebuffer
objs=("$out/BeringiaFramebuffer.o" "$out/BeringiaPCI.o")
cxx=$BERINGIA_TOOLCHAIN/binx86/target-clang++
ld=$BERINGIA_TOOLCHAIN/toolchain/bin/x86_64-apple-darwin-ld
kheaders=$BERINGIA_SDK/System/Library/Frameworks/Kernel.framework/Headers

[ -x "$cxx" ] || die "no Darwin clang++ at $cxx"
[ -x "$ld" ] || die "no ld64 at $ld"
[ -d "$BERINGIA_KDK_EXTENSIONS/System.kext/PlugIns" ] || die "no KDK extensions at $BERINGIA_KDK_EXTENSIONS"
mkdir -p "$bundle/Contents/MacOS"

log "compiling BeringiaFramebuffer and BeringiaPCI (thin x86_64, macOS kernel ABI)"
for src in BeringiaFramebuffer BeringiaPCI; do
	"$cxx" \
		--target=x86_64-apple-macos26.0 -arch x86_64 -mmacosx-version-min=26.0 \
		-std=gnu++2b -fsized-deallocation -fapple-kext -static -mkernel -msoft-float \
		-O2 -g -c "$here/$src.cpp" -o "$out/$src.o" \
		-nostdlibinc -fno-builtin -fno-common -fsigned-bitfields -fmerge-all-constants \
		-fc++-static-destructors=none -fno-stack-protector \
		-DAPPLE -DKERNEL -DKERNEL_PRIVATE -DPRIVATE -D__MACHO__=1 \
		-DXNU_TARGET_OS_OSX -DXNU_PLATFORM_MacOSX -DLIBKERNCPP -DIOKIT \
		-Dx86_64 -DX86_64 -D__X86_64__ -DLP64 -DPAGE_SIZE_FIXED \
		-DCURRENT_MACHINE_CONFIG_LC=none \
		-I"$BERINGIA_ROOT/xnu/include" -isystem "$kheaders" -isystem "$BERINGIA_SDK/usr/include" \
		-Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers \
		-Wno-deprecated-declarations
done

log "linking MH_KEXT_BUNDLE"
"$ld" -arch x86_64 -macosx_version_min 26.0 -kext -static \
	-pagezero_size 0 -headerpad 152 -o "$exe" "${objs[@]}"
cp "$here/Info.plist" "$bundle/Contents/Info.plist"
python3 -I -c 'import plistlib, sys; plistlib.load(open(sys.argv[1], "rb"))' "$bundle/Contents/Info.plist"

headers=$($LLVM_BIN/llvm-objdump --macho --private-headers "$exe")
grep -Eq 'MH_MAGIC_64 +X86_64 +ALL +0x00 +KEXTBUNDLE' <<<"$headers" ||
	die "output is not a thin x86_64 MH_KEXT_BUNDLE"
printf '%s\n' "$headers" | sed -n '1,5p'

# IOPCIDevice's reserved vtable slots are exported by the kernel's IOPCIFamily but not by the KDK kext's export list;
# kcinject resolves them (they are in the KC), so the subclass is buildable.
python3 -I "$BERINGIA_ROOT/tools/kc/kpibind.py" --strict --allow '_ZN11IOPCIDevice22_RESERVEDIOPCIDevice' \
	"$exe" "$bundle/Contents/Info.plist" "$BERINGIA_KDK_EXTENSIONS" "$out/BeringiaFramebuffer.bound"
log "built $bundle"
