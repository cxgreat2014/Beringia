#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# One experiment, start to finish, as one command: boot a fresh copy of the golden image (optionally patched), take
# screenshots, dump the threads of chosen processes with their user stacks, stop, harvest macOS's own logs, and write
# summary.json / summary.txt into the run directory. Run as root (wsl -u root).
#
#   scripts/run/experiment.sh [options]
#     --note TEXT          what the experiment tests (meta.txt, summary)
#     --shots T1,T2,...    seconds after the modules are loaded at which to take a screenshot (default 60,120,180)
#     --dump P1,P2,...     after the last screenshot, dump the threads of these processes (process-name substrings)
#     --image-patch CMD    before booting, run the shell command line CMD with the run's copy mounted read-write and its
#                          mount point appended as the last argument, from the repository root; for example
#                          --image-patch "python3 -I tools/disk/launchd-disabled.py com.apple.InstallerProgress"
#     --grep REGEX         put unified-log messages (or processes) matching REGEX into the summary; repeatable
#     --settle SECS        wait this long after the last screenshot before stopping, so logd persists more (default 30)
#     --keep-running       leave XNU running at the end (no stop, no harvest)
#
# Needs BERINGIA_GOLDEN (the image is copied by scripts/run/disk-copy.sh) and the usual boot.sh settings (local.env,
# BERINGIA_BOOT_ARGS, BERINGIA_KERNEL, ...). Optional: BERINGIA_APFS_KO (linux-apfs-rw, for --image-patch and the
# harvest), BERINGIA_UNIFIEDLOG (log decoder), BERINGIA_DYLD_CACHE (to name user frames; default: the extracted
# system tree's cache under $BERINGIA_ASSETS/final-root). The evidence is in the run directory
# ($BERINGIA_WIN_DIR/runs/<id>/); the disk copy stays in $BERINGIA_WORK/runs/<id>/ until disk-copy.sh prunes it.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"
[ "$(id -u)" = 0 ] || die "run as root (wsl -u root)"

note= shots=60,120,180 dumps= patch= settle=30 keep=0
greps=()
while [ $# -gt 0 ]; do
	case $1 in
	--note) note=$2; shift 2 ;;
	--shots) shots=$2; shift 2 ;;
	--dump) dumps=$2; shift 2 ;;
	--image-patch) patch=$2; shift 2 ;;
	--grep) greps+=(--grep "$2"); shift 2 ;;
	--settle) settle=$2; shift 2 ;;
	--keep-running) keep=1; shift ;;
	*) die "unknown option $1 (see the header of $0)" ;;
	esac
done
: "${BERINGIA_GOLDEN:?set BERINGIA_GOLDEN (local.env)}"
: "${BERINGIA_WIN_DIR:?set BERINGIA_WIN_DIR (local.env): the evidence goes there}"
grep -q -E '^beringia_(xnu|host) ' /proc/modules && die "a run is still active: end it with scripts/run/stop.sh"
root=$BERINGIA_ROOT
cache=${BERINGIA_DYLD_CACHE:-$BERINGIA_ASSETS/final-root/System/Library/dyld/dyld_shared_cache_x86_64h}

apfs_loaded() {
	grep -q '^apfs ' /proc/modules && return
	if [ -n "${BERINGIA_APFS_KO:-}" ]; then insmod "$BERINGIA_APFS_KO"; else modprobe apfs; fi
}

run_id=$(date +%Y%m%d-%H%M%S)
run_dir=$BERINGIA_WIN_DIR/runs/$run_id
log "experiment $run_id${note:+: $note}"
copy=$("$root/scripts/run/disk-copy.sh" "$run_id")
if [ -n "$patch" ]; then
	apfs_loaded
	log "patching the disk copy: $patch"
	unshare -m --propagation private bash -c 'set -e; m=$(mktemp -d); mount -t apfs -o readwrite,loop "$1" "$m"
		trap "sync; umount \"$m\"; rmdir \"$m\"" EXIT
		grep -q " $m apfs rw" /proc/mounts || { echo "error: the copy did not mount read-write" >&2; exit 1; }
		cd "$3"; MNT=$m bash -c "$2 \"\$MNT\""' _ "$copy" "$patch" "$root" || die "the image patch failed"
fi

env BERINGIA_RUN_ID="$run_id" BERINGIA_DISK="$copy" BERINGIA_NOTE="$note" BERINGIA_NO_BUILD=1 \
	"$root/scripts/run/boot.sh" 1 > /dev/null
t0=$(date +%s)
[ -d "$run_dir" ] || die "boot.sh made no run directory $run_dir"
[ -z "$patch" ] || echo "image_patch=$patch" >> "$run_dir/meta.txt"
log "running; run directory $run_dir"

panicked() { dmesg | grep -q "XNU PANIC"; }
IFS=, read -r -a times <<< "$shots"
for t in "${times[@]}"; do
	while [ $(( $(date +%s) - t0 )) -lt "$t" ]; do
		panicked && break 2
		sleep 2
	done
	python3 -I "$root/tools/debug/fbshot.py" "$run_dir/fb-${t}s.png" > /dev/null || log "screenshot at ${t}s failed"
done
if panicked; then
	log "XNU panicked"
fi

if [ -n "$dumps" ] && ! panicked; then
	dmesg > "$run_dir/dmesg-before-dumps.txt"
	IFS=, read -r -a procs <<< "$dumps"
	for p in "${procs[@]}"; do
		dmesg -C
		echo "$p" > /sys/module/beringia_xnu/parameters/dump
		sleep 3
		dmesg | grep -a "BERINGIA dump" | sed 's/^.*BERINGIA dump: //' > "$run_dir/dump-$p.txt" || true
		if [ -f "$cache" ]; then
			python3 -I "$root/tools/debug/dyldsym.py" "$cache" < "$run_dir/dump-$p.txt" > "$run_dir/dump-$p-sym.txt" ||
				log "symbolizing the dump of $p failed"
		fi
	done
fi

if [ "$keep" = 1 ]; then
	log "left running (--keep-running): end it with scripts/run/stop.sh, then tools/debug/harvest.sh"
	exit 0
fi
panicked || sleep "$settle"
"$root/scripts/run/stop.sh" > /dev/null
apfs_loaded || log "no apfs module: the harvest will fail"
"$root/tools/debug/harvest.sh" "$copy" "$run_dir/macos-logs" > /dev/null 2>&1 || log "harvest failed"
python3 -I "$root/tools/debug/run-summary.py" "$run_dir" "${greps[@]}"
