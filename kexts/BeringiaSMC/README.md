# BeringiaSMC

`BeringiaSMC` is a deliberately narrow `AppleSMCFamily` compatibility service
for a hosted machine with no physical SMC. Apple's `Dont Steal Mac OS X.kext`
waits for an `AppleSMC` service and reads the 32-byte `OSK0` and `OSK1`
values before registering the DSMOS page transformer. **The key is not part of
this kext**: you supply it from your own Mac (`BERINGIA_OSK_FILE`, see
[docs/setup/macos-assets.md](../../docs/setup/macos-assets.md)).

This is not a general SMC emulation. Fan, battery, temperature, power-button,
and hardware-interrupt functionality are out of scope. Use VirtualSMC if a
future milestone needs those interfaces.

## ABI approach

The KDK does not include the complete private `AppleSMCFamily` declaration, so
the kext does not guess its object size or vtable layout:

1. `OSTypeAlloc(AppleSMC)` invokes Apple's concrete-class metaclass allocator
   (`AppleSMCFamily` itself is abstract and its allocator returns `nullptr`).
2. Before anything else runs, the object's vtable is checked: slot `0x850` must
   still point to Apple's linked `smcReadKey`, and the table must end exactly
   after 298 entries (`0x950` bytes, the 25A353 `AppleSMC.kext` layout). An
   unexpected KDK fails the load instead of corrupting memory.
3. Apple's directly linked `AppleSMCFamily::init()` and `start()` initialize the
   object without hardware probing or importing the base-class vtable.
4. The instance gets its own copy of the vtable, in the kext's static data:
   `smcReadKey` (`0x850`) answers `OSK0`/`OSK1`, and every other SMC entry point
   (`0x858`–`0x8c8`: `smcReadPKey` … `smcHandleYPCEvent`) reports
   "key not found" (`0x84`). Those methods would otherwise dereference
   `AppleSMC`'s command gate, which never exists because `AppleSMC::init/start`
   never ran. Results are 8-bit SMC codes (0 = success), as the real methods
   return.
5. The object is named `AppleSMC` and registered. DSMOS then matches it and
   receives the key through its normal `smcReadKey` call.

Because the registered object points into this kext's image, the kext refuses
to unload (its kmod stop routine fails).

**Only DSMOS may use it.** Other `AppleSMC` clients in the KDK
(`AppleSmartBatteryManager`, `KextAudit`, `AppleIntelPCHPMC`, ...) expect a real
SMC; do not add them to the kernel collection alongside this kext.

## The key

The loader reads the file named by `BERINGIA_OSK_FILE` (64 printable
characters: OSK0, then OSK1) and passes it as the device-tree property
`/efi/platform/beringia-osk`. `start()` copies it into the kext once and removes
the property, before user space could read the device tree with `ioreg`.
Without it the kext logs `no DSMOS key supplied` and publishes nothing, and
DSMOS keeps waiting. Neither the source nor the built kext contains the key.

## Build and load

```bash
cd ~/Beringia
kexts/BeringiaSMC/build.sh
```

The bundle declares `com.apple.driver.AppleSMC` and the IOKit/libkern/Mach KPIs.
Include it after `AppleSMC.kext` and `Dont Steal Mac OS X.kext` in the prelinked
kernel's kext list. Its personality uses the same `IOResources`/`IOKit` match as
BeringiaDisk, so it starts during early IOKit setup.

Expected console evidence:

```text
xnu: DSMOS key supplied, for BeringiaSMC
BeringiaSMC: published AppleSMC for DSMOS (OSK0/OSK1; other SMC calls report key-not-found)
DSMOS has arrived
```
