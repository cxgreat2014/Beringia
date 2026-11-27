#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy macOS's own logs out of a system image after a run: launchd's log, system.log, crash reports (.ips) and the
# unified log (tracev3 + uuidtext), and decode the unified log to JSON lines if a decoder is available.
#
#   sudo tools/debug/harvest.sh [--clear] [IMAGE] [OUTDIR]
#
# OUTDIR defaults to "macos-logs" in the newest run directory under $BERINGIA_WIN_DIR/runs, IMAGE to the disk that run
# booted (its meta.txt: with BERINGIA_GOLDEN, the run's own copy), else $BERINGIA_DISK. --clear then empties the
# image's unified log, launchd log and crash reports, for an image that is booted again (a per-run copy needs none).
#
# The image is mounted with the linux-apfs-rw driver ($BERINGIA_APFS_KO, or an apfs module already loaded/installed)
# in a private mount namespace, so the mount does not leak into other namespaces (WSL services hold copies of shared
# mounts, which keeps the loop device and the module busy). Refuses to run while beringia_xnu is loaded: XNU has the
# image open read-write.
#
# Unified log decoding uses Mandiant's macos-UnifiedLogs (Apache-2.0) "unifiedlog_iterator" from $BERINGIA_UNIFIEDLOG
# (path to the binary). It runs on a Linux scratch copy (much faster than on /mnt/c) and leaves unifiedlog.jsonl.gz
# in OUTDIR, one JSON object per log entry. For example:
#   zcat OUTDIR/unifiedlog.jsonl.gz | jq -c 'select(.process|test("WindowServer")) | [.timestamp,.pid,.message]'
set -euo pipefail
. "$(dirname "$0")/../../scripts/lib/env.sh"

clear=0
if [ "${1:-}" = --clear ]; then
	clear=1
	shift
fi
[ "$(id -u)" = 0 ] || die "run as root (mount)"
grep -q '^beringia_xnu ' /proc/modules && die "beringia_xnu is loaded: end the run first (scripts/run/stop.sh)"
last=
if [ -n "${BERINGIA_WIN_DIR:-}" ] && [ -d "$BERINGIA_WIN_DIR/runs" ]; then
	# run ids are timestamps (YYYYmmdd-HHMMSS), so the last one in sort order is the newest run
	last=$(find "$BERINGIA_WIN_DIR/runs" -mindepth 1 -maxdepth 1 -type d | sort | tail -1)
fi
img=${1:-}
if [ -z "$img" ] && [ -n "$last" ] && [ -f "$last/meta.txt" ]; then
	img=$(sed -n 's/^disk=//p' "$last/meta.txt")
fi
[ -n "$img" ] && [ "$img" != "<none>" ] || img=${BERINGIA_DISK:-}
[ -n "$img" ] && [ -f "$img" ] || die "no image ${img:-given, none in the meta.txt of the newest run, no BERINGIA_DISK}"
out=${2:-}
if [ -z "$out" ]; then
	[ -n "$last" ] || die "no OUTDIR and no run directory under \$BERINGIA_WIN_DIR/runs"
	out=$last/macos-logs
fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
owner=$(stat -c %U "$(dirname "$out")")

if ! grep -q '^apfs ' /proc/modules; then
	if [ -n "${BERINGIA_APFS_KO:-}" ]; then
		insmod "$BERINGIA_APFS_KO"
	else
		modprobe apfs || die "no apfs module: set BERINGIA_APFS_KO to linux-apfs-rw's apfs.ko"
	fi
fi

mkdir -p "$BERINGIA_WORK/tmp"
scratch=$(mktemp -d "$BERINGIA_WORK/tmp/harvest.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
log "copying logs from $img"
unshare -m --propagation private bash -s "$img" "$scratch" "$clear" <<'EOF'
set -euo pipefail
img=$1 dst=$2 clear=$3
mnt=$(mktemp -d)
if [ "$clear" = 1 ]; then
	mount -t apfs -o readwrite,loop "$img" "$mnt"
else
	mount -t apfs -o ro,loop "$img" "$mnt"
fi
trap 'sync; umount "$mnt"; rmdir "$mnt"' EXIT
copy() {	# source relative to the volume root, destination
	if [ -e "$mnt/$1" ]; then
		mkdir -p "$2"
		cp -a "$mnt/$1/." "$2/"
	fi
}
copy private/var/log/com.apple.xpc.launchd "$dst/launchd"
for f in system.log install.log; do
	if [ -f "$mnt/private/var/log/$f" ]; then
		cp -a "$mnt/private/var/log/$f" "$dst/"
	fi
done
copy Library/Logs/DiagnosticReports "$dst/DiagnosticReports"
# the unified log reader wants uuidtext and diagnostics merged into one log-archive directory
copy private/var/db/uuidtext "$dst/logarchive"
copy private/var/db/diagnostics "$dst/logarchive"
if [ "$clear" = 1 ]; then
	# the logs, not the structure: logd recreates its files; uuidtext stays (it describes the binaries, not a boot).
	# A missing directory is not an error: the logs were copied above, and aborting here would lose them with $scratch.
	clear_dir() { dir=$1
		shift
		if [ -d "$dir" ]; then
			find "$dir" "$@" || echo "warning: could not empty $dir" >&2
		fi
	}
	clear_dir "$mnt/private/var/db/diagnostics" -type f \( -name '*.tracev3' -o -name '*.timesync' \) -delete
	clear_dir "$mnt/private/var/log/com.apple.xpc.launchd" -type f -name 'launchd.log*' -delete
	clear_dir "$mnt/Library/Logs/DiagnosticReports" -maxdepth 1 -type f -name '*.ips' -delete
	echo "cleared the image's unified log, launchd log and crash reports"
fi
EOF

if [ -n "${BERINGIA_UNIFIEDLOG:-}" ] && [ -x "$BERINGIA_UNIFIEDLOG" ]; then
	log "decoding the unified log"
	chown -R "$owner:" "$scratch"
	# in the scratch directory: the decoder also writes an issues.jsonl into the current directory
	if (cd "$scratch" && runuser -u "$owner" -- "$BERINGIA_UNIFIEDLOG" -m log-archive -i "$scratch/logarchive" \
		-o "$scratch/unifiedlog.jsonl" >"$scratch/unifiedlog.stderr" 2>&1); then
		gzip -1 "$scratch/unifiedlog.jsonl"
	else
		log "unifiedlog_iterator failed (see unifiedlog.stderr)"
	fi
fi
# timestamps only: OUTDIR is often on drvfs (/mnt/c), where owners and modes cannot be kept
cp -r --preserve=timestamps "$scratch/." "$out/"
chown -R "$owner:" "$out" 2>/dev/null || true
ls -la "$out"
