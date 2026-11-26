# tools/kc: the prelinked kernel (M4)

Beringia boots its kernel with Apple's kexts (from your KDK) as a legacy, kextcache-style **prelinked kernel**: the
kernel Mach-O plus `__PRELINK_TEXT` (the kexts) and `__PRELINK_INFO` (their Info.plists and addresses). XNU reads it
in `KLDBootstrap::readPrelinkedExtensions`, copies each kext into the kext map and slides it. The kexts are linked on
Linux with [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg)'s OcAppleKernelLib, the linker OpenCore uses to
inject kexts at boot. Background and measurements: [docs/research/m4-spike.md](../../docs/research/m4-spike.md).

```
scripts/kc/build.sh corecrypto pthread        # kexts in load order, names of bundles in $BERINGIA_KDK_EXTENSIONS
sudo BERINGIA_NO_BUILD=1 BERINGIA_KERNEL=$BERINGIA_BUILD/kc/prelinkedkernel scripts/run/boot.sh 40
```

| File | License | What |
|---|---|---|
| `skeleton.py` | GPL-2.0-only | kernel → empty prelinked kernel with the KDK's KPI pseudo-extensions |
| `kpibind.py` | GPL-2.0-only | binds a kext's imports through its declared dependencies (KPI aliases); reports the rest |
| `kcinject.c`, `Makefile` | BSD-3-Clause | links kexts into the prelinked kernel (derived from OpenCorePkg's TestKextInject) |
| `macho.py` | GPL-2.0-only | the Mach-O helpers the scripts share |

`scripts/kc/build.sh` fetches OpenCorePkg and acidanthera's audk (EDK2 headers) at the commits pinned in
`scripts/lib/env.sh` into `$BERINGIA_WORK/deps`; nothing from them, and nothing from Apple, is committed. The output
(`$BERINGIA_BUILD/kc/prelinkedkernel`) contains Apple's binaries: keep it local.
