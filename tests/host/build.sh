#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the x86_64 host-isolation tests adapted from xnulinux/tests/host.
set -euo pipefail

SOURCE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$HOME/beringia-work/build/host-tests"}
CC=${CC:-cc}

mkdir -p "$BUILD_DIR"

"$CC" -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread \
    "$SOURCE_DIR/memcheck.c" -o "$BUILD_DIR/memcheck"
"$CC" -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -c "$SOURCE_DIR/fpcheck.c" -o "$BUILD_DIR/fpcheck.o"
"$CC" -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -c "$SOURCE_DIR/fpcheck_asm.S" -o "$BUILD_DIR/fpcheck_asm.o"
"$CC" "$BUILD_DIR/fpcheck.o" "$BUILD_DIR/fpcheck_asm.o" \
    -o "$BUILD_DIR/fpcheck"

printf 'Built %s and %s\n' "$BUILD_DIR/memcheck" "$BUILD_DIR/fpcheck"
