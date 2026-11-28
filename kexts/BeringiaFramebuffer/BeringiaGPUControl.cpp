// SPDX-License-Identifier: APSL-2.0
/*
 * The AGDC vendor attributes IOPresentment and GPUWrangler ask for. Selector numbers, buffer sizes and layouts are
 * in AppleGraphicsDeviceControl.h, reconstructed from the KDK's AppleGraphicsDeviceControl and AppleParavirtGPU.
 */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include "BeringiaGPUControl.h"
#include "BeringiaPCI.h"

OSDefineMetaClassAndStructors(BeringiaGPUControl, AppleGraphicsDeviceControl);

void
BeringiaGPUControl::setFramebuffer(IOService *framebuffer)
{
	fFramebuffer = framebuffer;
}

bool
BeringiaGPUControl::start(IOService *provider)
{
	if (!AppleGraphicsDeviceControl::start(provider)) {
		return false;
	}
	IOLog("BeringiaGPUControl: AGDC for PCI %04x:%04x under %s\n", BERINGIA_PCI_VENDOR, BERINGIA_PCI_DEVICE,
	    provider ? provider->getName() : "?");
	return true;
}

IOReturn
BeringiaGPUControl::vendor_doDeviceAttribute(unsigned int selector, unsigned long *p1, unsigned long p2,
    unsigned long *p3, unsigned long *p4, IOExternalMethodArguments *args)
{
	switch (selector) {
	case kAGDCVendorInfo: {
		/* platformID is vendor | device << 16, as AppleParavirtGPU reports its 0xEEEE106B */
		AGDCVendorInfo *info;

		if (!p3 || !p4 || *p4 < sizeof(AGDCVendorInfo)) {
			return kIOReturnBadArgument;
		}
		info = (AGDCVendorInfo *)p3;
		bzero(info, sizeof(*info));
		info->version = 0x00010000;
		memcpy(info->name, "Beringia", sizeof("Beringia"));
		info->platformID = BERINGIA_PCI_VENDOR | ((UInt32)BERINGIA_PCI_DEVICE << 16);
		/* iface 2 keeps IOPresentment's map builder from asking for a board-id it would not recognise; the
		 * platform gate still checks platformID */
		info->iface = 2;
		*p4 = sizeof(*info);
		return kIOReturnSuccess;
	}
	case kAGDCGPUCapability: {
		AGDCGPUCapability *caps;

		if (!p3 || !p4 || *p4 < sizeof(AGDCGPUCapability)) {
			return kIOReturnBadArgument;
		}
		caps = (AGDCGPUCapability *)p3;
		bzero(caps, sizeof(*caps));
		caps->count20 = 1;
		caps->count24 = 1;
		caps->count28 = 1;
		caps->count2c = 1;
		/* the fields hold services, not IDs: the base class calls getRegistryEntryID() on them (run 20261128-121204
		 * faulted with a registry ID as the object) */
		caps->framebufferCount = fFramebuffer ? 1 : 0;
		caps->providerID = (UInt64)(uintptr_t)getProvider();
		if (fFramebuffer) {
			caps->fbIDs[0] = (UInt64)(uintptr_t)fFramebuffer;
		}
		*p4 = sizeof(*caps);
		return kIOReturnSuccess;
	}
	default:
		return kIOReturnUnsupported;
	}
}
