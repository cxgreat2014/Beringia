#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The header-only SDK root XNU's kernel build compiles and links against ($BERINGIA_FAKEROOT):
#   AvailabilityVersions, libplatform and libdispatch headers (DroidVM's pinned reference sources),
#   XNU's own exported headers (installhdrs of $BERINGIA_XNU_SRC), and libfirehose_kernel.a for x86_64.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

tc=$BERINGIA_TOOLCHAIN
dv=$BERINGIA_DROIDVM
refs=${BERINGIA_REFS:-$dv/out/x12377}      # refer-libdispatch, refer-libplatform, refer-AvailabilityVersions
out=$BERINGIA_FAKEROOT
logs=$BERINGIA_BUILD/logs
mkdir -p "$out" "$logs"
chmod -R u+w "$out" 2>/dev/null || true

if [ ! -e "$out/usr/include/AvailabilityVersions.h" ]; then
	log "AvailabilityVersions"
	make -C "$refs/refer-AvailabilityVersions" install -j"$BERINGIA_JOBS" OBJROOT="$BERINGIA_BUILD/availability" \
		SYMROOT="$BERINGIA_BUILD/availability" DSTROOT="$out" CMAKE=/usr/bin/cmake NINJA=/usr/bin/ninja > "$logs/availability.log" 2>&1
fi
for f in usr/local/libexec/availability.pl usr/include/AvailabilityVersions.h; do
	mkdir -p "$BERINGIA_SDK/$(dirname "$f")"
	cp -f "$out/$f" "$BERINGIA_SDK/$f"
done

log "libplatform / libdispatch headers"
python3 -I - "$refs" "$out" <<'PY'
import shutil, subprocess, sys
from pathlib import Path
refs, out = Path(sys.argv[1]), Path(sys.argv[2])
for src, dst in (('include', 'usr/local/include'), ('private', 'usr/local/include'),
                 ('private/platform', 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform')):
    shutil.copytree(refs / 'refer-libplatform' / src, out / dst, dirs_exist_ok=True, symlinks=True)
os_dir = out / 'usr/local/include/os'
os_dir.mkdir(parents=True, exist_ok=True)
for h in (refs / 'refer-libdispatch/os').glob('*.h'):
    r = subprocess.run(['unifdef', '-DKERNEL=1', '-DOS_FIREHOSE_SPI=1', '-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1',
                        '-UOS_VOUCHER_ACTIVITY_SPI', str(h)], capture_output=True)
    if r.returncode not in (0, 1):
        raise SystemExit(r.stderr.decode())
    (os_dir / h.name).write_bytes(r.stdout)
(out / 'AppleInternal').mkdir(exist_ok=True)
PY

log "XNU installhdrs"
BERINGIA_BUILD=$BERINGIA_BUILD/hdrs bash "$BERINGIA_ROOT/scripts/xnu/build.sh" installhdrs > "$logs/installhdrs.log" 2>&1 ||
	die "installhdrs failed ($logs/installhdrs.log)"
chmod -R u+w "$out"
cp -a "$BERINGIA_BUILD/hdrs/xnu/dst/." "$out/"

log "libfirehose_kernel.a (x86_64)"
k=$out/System/Library/Frameworks/Kernel.framework/Versions/A
cc=$tc/binx86/target-clang
mkdir -p "$BERINGIA_BUILD/firehose" "$out/usr/local/lib/kernel-x86_64"
GUNYAH_APPLE_TARGET=x86_64-apple-macos26.0 "$cc" -target x86_64-apple-macos26.0 -isysroot "$BERINGIA_SDK" -ffreestanding \
	-nostdinc -fno-stack-protector -mno-implicit-float -fblocks -O2 -std=gnu11 -DKERNEL=1 -DDISPATCH_USE_DTRACE=0 \
	-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 -DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -Wno-packed -Wno-deprecated-declarations \
	-I "$refs/refer-libdispatch" -I "$k/PrivateHeaders" -I "$k/Headers" -I "$out/usr/local/include/os" \
	-I "$out/usr/local/include/firehose" -I "$BERINGIA_XNU_SRC/EXTERNAL_HEADERS" -I "$BERINGIA_SDK/usr/include" \
	-I "$("$cc" -print-resource-dir)/include" \
	-c "$refs/refer-libdispatch/src/firehose/firehose_buffer.c" -o "$BERINGIA_BUILD/firehose/firehose_buffer.o"
rm -f "$out/usr/local/lib/kernel-x86_64/libfirehose_kernel.a"
"$tc/toolchain/bin/x86_64-apple-darwin-ar" rcs "$out/usr/local/lib/kernel-x86_64/libfirehose_kernel.a" \
	"$BERINGIA_BUILD/firehose/firehose_buffer.o"
log "fakeroot ready: $out"
