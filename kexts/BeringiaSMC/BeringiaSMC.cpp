// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaSMC: the small AppleSMCFamily surface needed by DSMOS on a host with
 * no physical SMC. Apple's Dont Steal Mac OS X kext waits for an "AppleSMC"
 * service and calls smcReadKey() for OSK0/OSK1. We allocate a concrete AppleSMC
 * through its own metaclass, initialize it only as an AppleSMCFamily (skipping
 * hardware probing), and give this one instance a vtable of its own in which
 * the SMC interface answers without hardware.
 *
 * The object is only half an AppleSMC: AppleSMC::init/start never ran, so its
 * command gate (and everything else that talks to the SMC) is missing. Every
 * SMC entry point that would reach it is therefore replaced: smcReadKey answers
 * OSK0/OSK1, the rest report "key not found". Clients other than DSMOS
 * (AppleSmartBatteryManager, KextAudit, AppleIntelPCHPMC, ...) still must not
 * be added to the kernel collection with this kext: they expect a real SMC.
 *
 * The layout is checked, not assumed: slot 0x850 must be Apple's linked
 * smcReadKey and the table must end exactly after kVtableSlots entries (the
 * 25A353 AppleSMC.kext vtable, 0x950 bytes). An unexpected KDK fails the load.
 *
 * The key itself is not part of Beringia: it is Apple's text, which the SMC of
 * every Intel Mac returns for OSK0 and OSK1, and the user supplies it from their
 * own Mac (BERINGIA_OSK_FILE, docs/setup/macos-assets.md). The loader passes it as
 * /efi/platform/beringia-osk; start() takes it and removes that property before
 * user space can read the device tree. Without it, no AppleSMC is published and
 * DSMOS keeps waiting.
 */
#include <TargetConditionals.h>
#undef TARGET_OS_OSX
#define TARGET_OS_OSX 1

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

/* The KDK does not ship the complete private AppleSMCFamily header. This
 * declaration is used only for metaclass allocation; Apple's init/start are
 * linked directly and virtual dispatch uses the vtable built below. */
class AppleSMCFamily : public IOService {
	OSDeclareDefaultStructors(AppleSMCFamily)
};
class AppleSMC : public AppleSMCFamily {
	OSDeclareDefaultStructors(AppleSMC)
};

extern "C" bool appleSMCInit(AppleSMCFamily *, OSDictionary *)
	__asm__("__ZN14AppleSMCFamily4initEP12OSDictionary");
extern "C" bool appleSMCStart(AppleSMCFamily *, IOService *)
	__asm__("__ZN14AppleSMCFamily5startEP9IOService");
extern "C" bool appleSMCReadKey(AppleSMCFamily *, UInt32, UInt64, void *)
	__asm__("__ZN14AppleSMCFamily10smcReadKeyEjyPv");

namespace {
/* AppleSMC's vtable in the 25A353 KDK: 298 entries from the address point (0x950 bytes), then 0. */
constexpr size_t kVtableSlots = 0x950 / sizeof(uintptr_t);
constexpr size_t kReadKeySlot = 0x850 / sizeof(uintptr_t);
/* 0x858..0x8c8: smcReadPKey .. smcHandleYPCEvent, every other SMC entry point (all reach the missing gate). */
constexpr size_t kFirstOtherSlot = 0x858 / sizeof(uintptr_t);
constexpr size_t kLastOtherSlot = 0x8c8 / sizeof(uintptr_t);
constexpr UInt32 kOSK0 = 0x4f534b30;
constexpr UInt32 kOSK1 = 0x4f534b31;
constexpr const char *kOSKProperty = "beringia-osk";

/* SMC result codes (an 8-bit SMCResult; callers test the low byte, 0 is success). */
constexpr UInt32 kSMCSuccess = 0x00;
constexpr UInt32 kSMCKeyNotFound = 0x84;
constexpr UInt32 kSMCBadArgument = 0x89;

/*
 * The replacement vtable. Static, so it lives exactly as long as this kext's image; the kext refuses to unload
 * (beringia_smc_stop) because the registered object keeps pointing at it.
 */
uintptr_t sVtable[kVtableSlots];
/* OSK0 then OSK1, as supplied by the user; set once by takeOSK(). */
char sOSK[64];
bool sStarted;

UInt32
readOSKey(AppleSMCFamily *, UInt32 key, UInt64 size, void *buffer)
{
	if (!buffer || size != 32)
		return kSMCBadArgument;
	if (key == kOSK0) {
		memcpy(buffer, sOSK, 32);
		return kSMCSuccess;
	}
	if (key == kOSK1) {
		memcpy(buffer, sOSK + 32, 32);
		return kSMCSuccess;
	}
	return kSMCKeyNotFound;
}

/* The user's key from the device tree, once; the property is removed either way (ioreg can read the device tree). */
bool
takeOSK()
{
	IORegistryEntry *platform = IORegistryEntry::fromPath("/efi/platform", gIODTPlane);
	OSData *key;
	bool ok = false;

	if (!platform)
		return false;
	key = OSDynamicCast(OSData, platform->getProperty(kOSKProperty));
	if (key && key->getLength() == sizeof(sOSK)) {
		memcpy(sOSK, key->getBytesNoCopy(), sizeof(sOSK));
		ok = true;
	}
	platform->removeProperty(kOSKProperty);
	platform->release();
	return ok;
}

/* For every other SMC entry point, whatever its parameters (SysV: the caller owns them; none returns a struct). */
UInt32
noSMC()
{
	return kSMCKeyNotFound;
}
}

class BeringiaSMC : public IOService {
	OSDeclareDefaultStructors(BeringiaSMC)

	AppleSMCFamily *fSMC = nullptr;

public:
	bool start(IOService *provider) override;
	void free() override;
};

OSDefineMetaClassAndStructors(BeringiaSMC, IOService)

bool
BeringiaSMC::start(IOService *provider)
{
	const uintptr_t *original;

	/* one AppleSMC per boot, however often the personality matches */
	if (sStarted)
		return false;
	sStarted = true;
	IOLog("BeringiaSMC: start\n");
	if (!takeOSK()) {
		IOLog("BeringiaSMC: no DSMOS key supplied (BERINGIA_OSK_FILE, see docs/setup/macos-assets.md); "
		    "not publishing AppleSMC, so DSMOS will keep waiting\n");
		return false;
	}
	if (!IOService::start(provider)) {
		IOLog("BeringiaSMC: IOService::start failed\n");
		return false;
	}
	fSMC = OSTypeAlloc(AppleSMC);
	if (!fSMC) {
		IOLog("BeringiaSMC: AppleSMC allocation failed\n");
		return false;
	}
	/* Check the layout before Apple's init/start run, so no failure below leaves a half-started family behind. */
	original = *reinterpret_cast<uintptr_t **>(fSMC);
	if (original[kReadKeySlot] != reinterpret_cast<uintptr_t>(appleSMCReadKey) ||
	    !original[kVtableSlots - 1] || original[kVtableSlots]) {
		IOLog("BeringiaSMC: unexpected AppleSMC vtable (slot %#zx = %#llx, last %#llx, end %#llx); not starting\n",
		    kReadKeySlot * sizeof(uintptr_t), (unsigned long long)original[kReadKeySlot],
		    (unsigned long long)original[kVtableSlots - 1], (unsigned long long)original[kVtableSlots]);
		OSSafeReleaseNULL(fSMC);
		return false;
	}
	if (!appleSMCInit(fSMC, nullptr)) {
		IOLog("BeringiaSMC: AppleSMCFamily::init failed\n");
		OSSafeReleaseNULL(fSMC);
		return false;
	}
	if (!fSMC->attach(provider)) {
		IOLog("BeringiaSMC: attach failed\n");
		OSSafeReleaseNULL(fSMC);
		return false;
	}
	if (!appleSMCStart(fSMC, provider)) {
		IOLog("BeringiaSMC: AppleSMCFamily::start failed\n");
		fSMC->detach(provider);
		OSSafeReleaseNULL(fSMC);
		return false;
	}

	memcpy(sVtable, original, sizeof(sVtable));
	sVtable[kReadKeySlot] = reinterpret_cast<uintptr_t>(readOSKey);
	for (size_t i = kFirstOtherSlot; i <= kLastOtherSlot; i++)
		sVtable[i] = reinterpret_cast<uintptr_t>(noSMC);
	*reinterpret_cast<uintptr_t **>(fSMC) = sVtable;

	fSMC->setName("AppleSMC");
	fSMC->registerService();
	IOLog("BeringiaSMC: published AppleSMC for DSMOS (OSK0/OSK1; other SMC calls report key-not-found)\n");
	return true;
}

void
BeringiaSMC::free()
{
	/* Our reference only: the registry keeps the published object, and sVtable is static. */
	OSSafeReleaseNULL(fSMC);
	IOService::free();
}

extern "C" {
kern_return_t
beringia_smc_start(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

kern_return_t
beringia_smc_stop(kmod_info_t *, void *)
{
	/* The AppleSMC object published by start() points at sVtable and readOSKey in this image. */
	return KERN_FAILURE;
}

KMOD_EXPLICIT_DECL(com.beringia.smc, "1.0.0",
    beringia_smc_start, beringia_smc_stop)
}
