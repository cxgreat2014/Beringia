# tools/kc: the prelinked kernel (M4)

Beringia boots its kernel with Apple's kexts (from your KDK) as a legacy, kextcache-style **prelinked kernel**: the
kernel Mach-O plus `__PRELINK_TEXT` (the kexts) and `__PRELINK_INFO` (their Info.plists and addresses). XNU reads it
in `KLDBootstrap::readPrelinkedExtensions`, copies each kext into the kext map and slides it. The kexts are linked on
Linux with [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg)'s OcAppleKernelLib, the linker OpenCore uses to
inject kexts at boot. Background and measurements: [docs/research/m4-spike.md](../../docs/research/m4-spike.md).

```
scripts/kc/build.sh corecrypto pthread        # kexts in load order, names of bundles in $BERINGIA_KDK_EXTENSIONS
scripts/kc/build.sh --closure com.apple.kec.pthread com.apple.filesystems.apfs
sudo BERINGIA_NO_BUILD=1 BERINGIA_KERNEL=$BERINGIA_BUILD/kc/prelinkedkernel scripts/run/boot.sh 40
```

`--closure` computes dependencies and load order with `deps.py`. As XNU does, it prefers
`OSBundleLibraries_x86_64` over the generic list; this is important for Apple's security kexts, whose generic lists
contain Apple-silicon-only dependencies. A bundle path relative to `$BERINGIA_KDK_EXTENSIONS` (including a PlugIns
path) is also accepted.

To add a kext built in this repository, build it first (for example `kexts/BeringiaDisk/build.sh`), then append its
bundle path relative to `$BERINGIA_KDK_EXTENSIONS` to the closure list. A complete example is in
[../../kexts/BeringiaDisk/README.md](../../kexts/BeringiaDisk/README.md) and
[../../docs/research/m5-userspace-bringup.md](../../docs/research/m5-userspace-bringup.md).

| File | License | What |
|---|---|---|
| `deps.py` | GPL-2.0-only | computes the dependency closure and load order, preferring `OSBundleLibraries_x86_64` |
| `skeleton.py` | GPL-2.0-only | kernel → empty prelinked kernel with the KDK's KPI pseudo-extensions |
| `kpibind.py` | GPL-2.0-only | binds a kext's imports through its declared dependencies (KPI aliases); reports the rest |
| `kcinject.c`, `Makefile` | BSD-3-Clause | links kexts into the prelinked kernel (derived from OpenCorePkg's TestKextInject) |
| `macho.py` | GPL-2.0-only | the Mach-O helpers the scripts share |

`scripts/kc/build.sh` fetches OpenCorePkg and acidanthera's audk (EDK2 headers) at the commits pinned in
`scripts/lib/env.sh` into `$BERINGIA_WORK/deps`; nothing from them, and nothing from Apple, is committed. The output
(`$BERINGIA_BUILD/kc/prelinkedkernel`) contains Apple's binaries: keep it local.
