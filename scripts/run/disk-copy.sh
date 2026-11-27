#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Make the disk for one run: a sparse copy of the golden image, which itself is never booted or written.
#
#   scripts/run/disk-copy.sh RUN_ID          prints the path of the copy
#
# BERINGIA_GOLDEN (local.env) is the golden image; keep it immutable (sudo chattr +i) so nothing can write it. The
# copy goes to $BERINGIA_WORK/runs/RUN_ID/system.img, on the Linux file system (a 40 GiB sparse image with ~30 GiB of
# data copies in well under a minute there; never under /mnt/c). Only the newest BERINGIA_KEEP_COPIES copies
# (default 3) are kept; older runs keep their directory, without the image. A run's own logs are then only its own,
# and an experiment that patches its disk (scripts/run/experiment.sh --image-patch) cannot affect the next one.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

run_id=${1:?usage: disk-copy.sh RUN_ID}
: "${BERINGIA_GOLDEN:?set BERINGIA_GOLDEN to the golden system image (local.env)}"
[ -f "$BERINGIA_GOLDEN" ] || die "no golden image at $BERINGIA_GOLDEN"
dir=$BERINGIA_WORK/runs/$run_id
copy=$dir/system.img
[ ! -e "$copy" ] || die "$copy exists already"
mkdir -p "$dir"
cp --sparse=always "$BERINGIA_GOLDEN" "$copy.tmp"
mv -f "$copy.tmp" "$copy"

keep=${BERINGIA_KEEP_COPIES:-3}
find "$BERINGIA_WORK/runs" -mindepth 2 -maxdepth 2 -name system.img -printf '%h\n' | sort | head -n "-$keep" |
	while read -r old; do rm -f "$old/system.img"; done
echo "$copy"
