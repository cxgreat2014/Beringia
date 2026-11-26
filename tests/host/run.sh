#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Adapted from xnulinux/tests/host/run.sh for the x86_64 host tests.
set -uo pipefail

SOURCE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$HOME/beringia-work/build/host-tests"}
DURATION=${1:-${DURATION:-60}}
MEMCHECK_GIB=${MEMCHECK_GIB:-2}
FPCHECK_CPU=${FPCHECK_CPU:-0}
RESULT_FILE=${RESULT_FILE:-"$BUILD_DIR/results.log"}

if [[ $# -gt 1 || ! $DURATION =~ ^[1-9][0-9]*$ ]]; then
    printf 'Usage: %s [SECONDS]\n' "$0" >&2
    printf 'Environment: MEMCHECK_GIB=2 FPCHECK_CPU=0 BUILD_DIR=... RESULT_FILE=...\n' >&2
    exit 2
fi

"$SOURCE_DIR/build.sh"
mkdir -p "$(dirname -- "$RESULT_FILE")"

(
    fp_pid=
    mem_pid=
    trap '[[ -z $fp_pid ]] || kill "$fp_pid" 2>/dev/null; [[ -z $mem_pid ]] || kill "$mem_pid" 2>/dev/null; exit 130' INT TERM

    printf 'Host-isolation tests: duration=%ss, memcheck=%s GiB, fpcheck CPU=%s\n' \
        "$DURATION" "$MEMCHECK_GIB" "$FPCHECK_CPU"
    printf 'No hosted XNU is started or controlled by this script.\n'
    printf '\n===== fpcheck and memcheck (running concurrently) =====\n'
    "$BUILD_DIR/fpcheck" --cpu "$FPCHECK_CPU" --seconds "$DURATION" &
    fp_pid=$!
    "$BUILD_DIR/memcheck" --size-gib "$MEMCHECK_GIB" --seconds "$DURATION" &
    mem_pid=$!

    fp_status=0
    mem_status=0
    wait "$fp_pid" || fp_status=$?
    wait "$mem_pid" || mem_status=$?

    printf '\n===== summary =====\n'
    printf 'fpcheck exit status: %d\n' "$fp_status"
    printf 'memcheck exit status: %d\n' "$mem_status"
    if ((fp_status == 0 && mem_status == 0)); then
        printf 'HOST ISOLATION TESTS: PASS\n'
        exit 0
    fi
    printf 'HOST ISOLATION TESTS: FAIL\n'
    exit 1
) 2>&1 | tee "$RESULT_FILE"
status=${PIPESTATUS[0]}
printf 'Results written to %s\n' "$RESULT_FILE" | tee -a "$RESULT_FILE"
exit "$status"
