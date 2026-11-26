#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build static x86_64 Mach-O test programs for the Beringia RAM-disk root.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../../scripts/lib/env.sh
. "$script_dir/../../scripts/lib/env.sh"

: "${BERINGIA_BUILD:=$BERINGIA_WORK/build}"
: "${LLVM_BIN:=/usr/lib/llvm-21/bin}"
cc=$BERINGIA_TOOLCHAIN/binx86/target-clang
objdump=${OBJDUMP:-$(command -v llvm-objdump-21 || true)}
if [[ -z $objdump && -x $LLVM_BIN/llvm-objdump ]]; then
	objdump=$LLVM_BIN/llvm-objdump
fi
[[ -x $cc ]] || die "Darwin compiler not found: $cc"
[[ -n $objdump && -x $objdump ]] || die "llvm-objdump-21 not found"

out=$BERINGIA_BUILD/init
target=x86_64-apple-macos26.0
mkdir -p -- "$out"

for name in launchd hello; do
	log "assembling $name.s"
	"$cc" -target "$target" -c "$script_dir/$name.s" -o "$out/$name.o"
	log "linking static $name"
	"$cc" -target "$target" -nostdlib -static -e _start \
		-Wl,-adhoc_codesign "$out/$name.o" -o "$out/$name"
	chmod 755 "$out/$name"

	# Keep the exact verification output as a build artifact and reject any
	# dynamic loader or dylib dependency, not just a missing LC_LOAD_DYLINKER.
	"$objdump" --macho --private-headers "$out/$name" \
		> "$out/$name.private-headers.txt"
	grep -Eq 'MH_EXECUTE|[[:space:]]EXECUTE[[:space:]]' "$out/$name.private-headers.txt" ||
		die "$name is not an MH_EXECUTE Mach-O"
	if grep -Eq 'LC_LOAD_DYLINKER|LC_LOAD(_WEAK)?_DYLIB' \
		"$out/$name.private-headers.txt"; then
		die "$name unexpectedly has a dynamic loader/library load command"
	fi
	printf 'verified static Mach-O (MH_EXECUTE, no dylinker): %s\n' "$out/$name"
done

printf '\nMach-O private headers (launchd):\n'
cat "$out/launchd.private-headers.txt"
printf '\nMach-O private headers (hello):\n'
cat "$out/hello.private-headers.txt"
printf '\nBinaries: %s %s\n' "$out/launchd" "$out/hello"
