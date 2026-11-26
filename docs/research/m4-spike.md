<!-- SPDX-License-Identifier: GPL-2.0-only -->
# M4 spike: linking Apple's kexts to Beringia's kernel (W1, decision G1)

2026-11-26. Question: can the kexts of the macOS 26 (25A353) KDK be linked, on Linux, to the kernel Beringia builds,
and do they run? Answer: **yes**. A legacy (kextcache-style, pre-macOS 11) prelinked kernel, with the kexts linked by
OpenCorePkg's OcAppleKernelLib, boots with `corecrypto` and `pthread` to "Still waiting for root device"
(run `20261126-214058`). Decision G1: route (a) below; (b) and (c) are not pursued.

## What was built

| Piece | What it does |
|---|---|
| `tools/kc/skeleton.py` | Turns the built kernel into an empty prelinked kernel: `__PRELINK_TEXT` right after `__LINKEDIT` (file and memory), holding the KDK's KPI symbol sets; `__PRELINK_INFO` last, holding the plist (`_PrelinkInfoDictionary` with the 13 `System.kext` pseudo-extensions, `_PrelinkKCID`). The kernel's bytes are unchanged (only the two segment commands). |
| `tools/kc/kpibind.py` | Binds a kext's imports through the dependencies its Info.plist declares (KPI aliases first, see below) and reports imports no declared dependency exports. |
| `tools/kc/kcinject.c` | A minimal driver for OcAppleKernelLib (`PrelinkedContextInit` / `PrelinkedInjectPrepare` / `PrelinkedInjectKext` / `PrelinkedInjectComplete`), derived from OpenCorePkg's TestKextInject without its kernel patches. |
| `scripts/kc/build.sh` | Fetches OpenCorePkg and acidanthera's audk (headers) at pinned commits, builds kcinject, makes the skeleton, thins/binds/links the named kexts: `scripts/kc/build.sh corecrypto pthread`. Boot with `BERINGIA_KERNEL=$BERINGIA_BUILD/kc/prelinkedkernel scripts/run/boot.sh`. |

The loader needed no change: it maps every segment and slides the header, so the extra segments come along.

## Findings

1. **XNU still boots legacy prelinked kernels on x86_64.** `KLDBootstrap::readStartupExtensions` reads
   `__PRELINK_INFO,__info` when it is not empty; hosted patch "report a bare hosted kernel as KCFormatStatic" already
   puts the kernel in the static format. Each kext is registered, copied into the kext map and slid by XNU itself
   (`slidePrelinkedExecutable`: no `_PrelinkLinkKASLROffsets`, so per-kext local relocations).
2. **Kexts run in the kext basement, not in `__PRELINK_TEXT`.** With `CONFIG_KEXT_BASEMENT`, the kext map is
   `[end of __TEXT - 2 GB, ...)` (unslid `0xffffff7f80e00000` here) and XNU requires each prelinked kext there
   (`setVMAttributes`, `kext_free`). Every executable gets two addresses: `_PrelinkExecutableSourceAddr` in
   `__PRELINK_TEXT` and `_PrelinkExecutableLoadAddr` in the basement; XNU copies one to the other. The first boot with
   load = source panicked in `vm_map_delete` (run `20261126-212949`).
3. **KPI pseudo-extensions with their symbol sets** (`__LINKEDIT`-only Mach-Os) give OpenCorePkg the KPI aliases
   and its anchor for the next load address. XNU treats them as interface kexts (fake kmod_info, no VM protection).
4. **KPI aliases must win over kernel symbols.** OpenCorePkg looks a name up in the kernel before the KPIs. XNU's
   link applies the alias list to the kernel's symbol table, so 108 of the KDK's 110 aliases already point to the
   same address; `_kalloc_type_var_impl` and `_kfree_type_var_impl` do not (the kernel also has internal functions
   with those names). `kpibind.py` renames aliased imports to their targets before linking.
5. **kalloc_type: no hang, by construction.** With KCFormatStatic, `kalloc_type_view_parse` only walks the kernel's
   own sections; kext views stay unprocessed and `kalloc_type_impl_external` / `kfree_type_impl_external` serve them
   from the default/data heaps (the path Apple uses for kexts outside the BootKC). DroidVM's hang came from mixing
   Apple's typed views into the startup parse, which this route never does.
6. **corecrypto's FIPS self-test passes after relinking**, all 20 tests including `fipspost_post_integrity`: no
   HMAC resealing was needed on this route (DroidVM needed it for its KCs).
7. **One XNU fix**: a legacy prelinked kext is started (`validateKextMapping` → `mach_vm_region_recurse` →
   `vm_kernel_addrhash`) before corecrypto registers, and corecrypto is one of them; hosted patch 0031 hashes
   without corecrypto until it registers. Then the network stack's AES needs corecrypto: corecrypto is required.
8. **pthread registers in time** (`loadKernelExternalComponents`, from `PE_init_iokit`): no late-registration
   fallback is needed (xnulinux needed one).

## Compatibility measurements

- **Symbols.** Every name the KDK's KPI symbol sets export is defined by our kernel, except 111 TightBeam (`_tb_*`)
  functions in `com.apple.kpi.private`: Apple's private IPC library, not in the open-source XNU. Only
  `ExclaveKextClient` and `ExclaveSEPManagerProxy` (Apple silicon) import them.
- **Whole KDK.** 394 kexts have an x86_64 slice; 341 resolve every import through their declared dependencies. Most
  of the rest import C++ metaclass symbols of indirect dependencies (`IOACPIPlatformDevice`, ...), which kxld and
  OpenCorePkg resolve through the dependency tree.
- **C++ ABI.** All 348 vtables of Apple's `kernel.development` (KDK) exist in ours with the same number of slots and
  the same function in every slot (24 210 slots); ours adds only BeringiaPlatform. IOService has 266 slots in both.
- **pthread**: 58 imports, all exported by its dependencies, 4 bound through aliases; 0 external relocations left,
  41 local relocations kept for XNU's slide; every call target is the intended kernel function.

## Routes not taken

- (b) XNU's own kxld (`CONFIG_KXLD`, `readBooterExtensions`): would also need the booter-extension memory map entries.
  Not needed now; may matter later for kexts loaded after boot.
- (c) Porting kxld to Linux: not needed.
- A fileset (MH_FILESET) KC: what Apple ships, but it needs chained fixups and a loader that maps a fileset; the
  legacy format is enough for M4 and keeps the kernel a plain MH_EXECUTE.

## Open items

- The security stack (M4-c): CoreTrust, AppleKeyStore, AppleImage4, AMFI, Sandbox, Quarantine, AppleSystemPolicy
  together, and the trust cache stub (patch 0029) turned into lazy initialisation.
- Struct layouts outside vtables (dSYM comparison) are not checked yet; same source and configuration, so differences
  can only come from hosted patches.
- `scripts/kc/build.sh` takes an explicit kext list in load order; the dependency closure (W2) is next.
