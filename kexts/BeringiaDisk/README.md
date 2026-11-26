# BeringiaDisk

`BeringiaDisk` is an `IOBlockStorageDevice` backed by one raw host file. It is
synchronous by design: an `IOLock` serializes requests, each request finishes in
the host before its IOKit completion runs, and a 1 MiB bounce buffer avoids
passing arbitrary XNU virtual addresses to Linux.

Apple's `IOBlockStorageDriver` attaches to the device and publishes the whole
`IOMedia`. The driver gets a 512-byte block size, the maximum valid block from
the host file size, media-present/fixed-media state, the read-only flag, and no
volatile write cache. The device also advertises virtual/internal interconnect
and Beringia vendor/product characteristics. Apple's GPT scheme can then publish
partition media with their content types, and APFS/HFS can probe those media in
the normal way. Use a raw whole-disk GPT image for a normal macOS system disk,
not a compressed DMG. The file size must be a nonzero multiple of 512 bytes.

## Host-call ABI

The layout is in `xnu/include/beringia/abi.h` and remains **ABI version 1**.
Calls 34 and 35 were already reserved with these structures; this implementation
fills in their semantics without changing the shared-memory layout or call
numbers. `a2` is zero for both calls. The host-call transport returns zero when
a request was processed, or `-EFAULT` when the request structure's physical
address itself is invalid.

* **35, `BERINGIA_CALL_BLOCK_INFO`**: `a1` is the physical address of
  `struct beringia_block_info`. The caller supplies `unit` (only unit 0 exists);
  the host fills `block_size = 512`, `size` in bytes (`0` means not configured),
  and `read_only`.
* **34, `BERINGIA_CALL_BLOCK_RW`**: `a1` is the physical address of
  `struct beringia_block_req`. `unit` is 0; `write` is
  `BERINGIA_BLOCK_READ` (0), `BERINGIA_BLOCK_WRITE` (1), or
  `BERINGIA_BLOCK_FLUSH` (2). For read/write, `offset` is a byte offset and up
  to 16 `{pa,len}` segments describe contiguous data in device order. On return,
  `result` is the total byte count or a negative Linux errno. Flush has no
  segments and returns `result = 0` on success.

The loader appends `beringia_hc=0x...` to XNU's command line. It is the same host
entry virtual address written to `beringia_hook`. BeringiaDisk reads it through
the exported `PE_parse_boot_argn` KPI and uses a three-argument SysV call stub
identical to `osfmk/x86_64/beringia.c`. This avoids importing `_beringia_call`,
which is a kernel global but is **not** in a KPI export set. No XNU source patch
is needed.

The kext allocates one physically contiguous `IOBufferMemoryDescriptor`: a
4096-byte request page followed by the bounce buffer. `getPhysicalSegment(...,
kIOMemoryMapperNone)` supplies their real physical addresses. A single lock
protects both areas. The host validates every range with `bh_phys` before a
write, temporarily restores Linux interrupts/preemption around sleeping file
I/O, and closes the file only after all hosted CPUs stop.

**Durability limitation:** positional `kernel_read`/`kernel_write` calls are
synchronous with respect to Linux and subsequent guest reads, but there is no
`bh_file_sync` helper yet. `BERINGIA_BLOCK_FLUSH` orders requests; it is not an
`fsync` promise to stable host storage. The kext consequently reports no write
cache. Add an fsync host helper before relying on crash durability.

## Loading at boot

`Info.plist` marks the bundle `Local-Root`, declares IOStorageFamily and the
IOKit/libkern/Mach KPIs, and has one personality with:

* `IOProviderClass = IOResources`
* `IOResourceMatch = IOKit`
* `IOClass = BeringiaDisk`

Once linked into the prelinked kernel, that personality loads and matches during
IOResources setup. `IOBlockStorageDevice` supplies the `device-type = Generic`
property used by Apple's `IOBlockStorageDriver` personality. BeringiaDisk delays
`registerService()` until the `IOBSD` resource appears (on a helper thread),
avoiding the early APFS/IOResources publication deadlock seen with immediate
registration. No user-space `kextload` is involved.

## Build

From the repository, with `local.env` selecting the Darwin toolchain, SDK and
KDK:

```bash
cd ~/Beringia
kexts/BeringiaDisk/build.sh
```

Output:

```text
~/beringia-work/build/kexts/BeringiaDisk/BeringiaDisk.kext/
  Contents/Info.plist
  Contents/MacOS/BeringiaDisk
```

The script compiles for `x86_64-apple-macos26.0`, links with the same ld64 used
for XNU, checks the Mach header with LLVM objdump, and runs `kpibind.py
--strict`. An equivalent manual inspection is:

```bash
llvm-objdump-21 --macho --private-headers \
  ~/beringia-work/build/kexts/BeringiaDisk/BeringiaDisk.kext/Contents/MacOS/BeringiaDisk
python3 -I tools/kc/kpibind.py --strict \
  ~/beringia-work/build/kexts/BeringiaDisk/BeringiaDisk.kext/Contents/MacOS/BeringiaDisk \
  ~/beringia-work/build/kexts/BeringiaDisk/BeringiaDisk.kext/Contents/Info.plist \
  "$BERINGIA_KDK_EXTENSIONS" \
  ~/beringia-work/build/kexts/BeringiaDisk/BeringiaDisk.bound
```

## Link into the prelinked kernel

The current `scripts/kc/build.sh` prefixes every kext argument with
`BERINGIA_KDK_EXTENSIONS`, so pass the custom bundle as a relative path (it does
not yet accept an absolute bundle path). This is the exact APFS-plus-disk
invocation used to test injection:

```bash
cd ~/Beringia
. scripts/lib/env.sh
kexts/BeringiaDisk/build.sh
mapfile -t apfs_kexts < <(
  python3 -I tools/kc/deps.py "$BERINGIA_KDK_EXTENSIONS" com.apple.filesystems.apfs
)
disk_kext=$(realpath --relative-to="$BERINGIA_KDK_EXTENSIONS" \
  "$BERINGIA_BUILD/kexts/BeringiaDisk/BeringiaDisk.kext")
scripts/kc/build.sh "${apfs_kexts[@]}" "$disk_kext"
```

The result is `$BERINGIA_BUILD/kc/prelinkedkernel`. For a full macOS boot, use
the same `"$disk_kext"` argument in your existing working security/APFS kext
list; the small APFS closure above is a link/storage test, not a replacement for
that full boot policy stack.

Rebuild the loader without loading it:

```bash
cd ~/Beringia
. scripts/lib/env.sh
make -C loader KDIR="$LINUX_KDIR" -j"$BERINGIA_JOBS"
```

## Boot test (for the main engineer)

Set `BERINGIA_DISK` to a raw whole-disk image and use the new prelinked kernel:

```bash
cd ~/Beringia
. scripts/lib/env.sh
export BERINGIA_DISK=/absolute/path/to/raw-gpt-system.img
export BERINGIA_KERNEL="$BERINGIA_BUILD/kc/prelinkedkernel"
make -C host KDIR="$LINUX_KDIR" -j"$BERINGIA_JOBS"
make -C loader KDIR="$LINUX_KDIR" -j"$BERINGIA_JOBS"
```

After the normal host setup, load the modules with the disk argument (example,
to be run only by the engineer controlling boot tests):

```bash
insmod "$BERINGIA_ROOT/host/beringia_host.ko" carve="$BERINGIA_CARVE"
insmod "$BERINGIA_ROOT/loader/beringia_xnu.ko" \
  kernel="$BERINGIA_KERNEL" disk="$BERINGIA_DISK" \
  boot_args="-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1"
```

Do not add `pmd0=`/`rd=md0` for this test. Let root-device discovery find the
APFS system volume, or supply the `rd=` identifier appropriate to the image if
your existing full-system boot requires it. First try a writable scratch copy;
the loader falls back to read-only if `O_RDWR` fails and reports that explicitly.

The checked-in `scripts/run/boot.sh` currently forwards `BERINGIA_RAMDISK` but
has no `BERINGIA_DISK` forwarding. It must gain a corresponding
`disk=/lib/firmware/...` module argument (or the direct `insmod` form above must
be used); merely setting `BERINGIA_DISK` with the unmodified script has no
effect. That runner file was intentionally not changed here.

Expected early evidence is the loader's `xnu: disk ... (block unit 0)` line and
`BeringiaDisk: ... blocks of 512 bytes`, followed by IOBlockStorageDriver media,
GPT partition, and APFS messages.

## Validation status

Validated without loading modules or booting XNU:

* the loader compiles successfully (its pre-existing `console_line` frame-size
  warning remains);
* the executable is a thin x86_64 `MH_KEXT_BUNDLE`;
* strict KPI binding reports zero unexported imports;
* OpenCore prelinked-kernel injection succeeds with IOStorageFamily and APFS.

Still untested on a live boot: host-call address handoff, physical bounce-buffer
I/O, IOBlockStorageDriver matching/media creation, APFS mounting, read/write
correctness, flush behavior, and read-only fallback. These require the main
engineer's controlled boot test.
