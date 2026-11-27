// SPDX-License-Identifier: APSL-2.0
/*
 * The virtual PCI display device: see BeringiaPCI.h for why it exists. Config space holds only what the display stack
 * reads: the identity (106b:eeee), the class code GPUWrangler requires (base class 3 = display controller), and the
 * subsystem id it copies into the GPU record.
 */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include "BeringiaPCI.h"

OSDefineMetaClassAndStructors(BeringiaPCIDevice, IOPCIDevice);

/* a 64-byte configuration space, little-endian dwords as a bridge would return them */
const UInt8
BeringiaPCIDevice::kConfig[64] = {
	0x6b, 0x10, 0xee, 0xee,         /* 0x00 vendor 0x106b, device 0xeeee */
	0x07, 0x00, 0x10, 0x00,         /* 0x04 command, status */
	0x00, 0x00, 0x00, 0x03,         /* 0x08 revision 0, prog-if 0, subclass 0, base class 3 (display) */
	0x00, 0x00, 0x00, 0x00,         /* 0x0c cache line, latency, header type 0, BIST */
	0x00, 0x00, 0x00, 0x00,         /* 0x10 BAR0: none (the framebuffer is not a BAR; see BeringiaFramebuffer) */
	0x00, 0x00, 0x00, 0x00,         /* 0x14 BAR1 */
	0x00, 0x00, 0x00, 0x00,         /* 0x18 BAR2 */
	0x00, 0x00, 0x00, 0x00,         /* 0x1c BAR3 */
	0x00, 0x00, 0x00, 0x00,         /* 0x20 BAR4 */
	0x00, 0x00, 0x00, 0x00,         /* 0x24 BAR5 */
	0x00, 0x00, 0x00, 0x00,         /* 0x28 cardbus CIS */
	0x6b, 0x10, 0xee, 0xee,         /* 0x2c subsystem vendor 0x106b, subsystem id 0xeeee */
	0x00, 0x00, 0x00, 0x00,         /* 0x30 ROM BAR */
	0x00, 0x00, 0x00, 0x00,         /* 0x34 capability pointer */
	0x00, 0x00, 0x00, 0x00,         /* 0x38 */
	0x00, 0x01, 0x00, 0x00,         /* 0x3c interrupt line, interrupt pin 1 (INTA) */
};

static void
setNumber(IOService *service, const char *key, UInt64 value, unsigned int bits)
{
	OSNumber *n = OSNumber::withNumber(value, bits);

	if (n) {
		service->setProperty(key, n);
		n->release();
	}
}

bool
BeringiaPCIDevice::start(IOService *provider)
{
	/* IOPCIDevice::start would ask the provider for a bridge; there is none, so publish what a bridge would */
	setNumber(this, "vendor-id", BERINGIA_PCI_VENDOR, 16);
	setNumber(this, "device-id", BERINGIA_PCI_DEVICE, 16);
	/* the word at byte offset 2, shifted left 16: GPUWrangler requires PCI base class 3 (display controller) */
	setNumber(this, "class-code", BERINGIA_PCI_CLASS, 24);
	setNumber(this, "revision-id", 0, 8);
	setNumber(this, "subsystem-vendor-id", BERINGIA_PCI_VENDOR, 16);
	setNumber(this, "subsystem-id", BERINGIA_PCI_DEVICE, 16);
	setName("BeringiaGPU");
	IOLog("BeringiaPCIDevice: PCI %04x:%04x, class %#x, published under %s\n", BERINGIA_PCI_VENDOR,
	    BERINGIA_PCI_DEVICE, BERINGIA_PCI_CLASS, provider ? provider->getName() : "?");
	return true;
}

UInt32
BeringiaPCIDevice::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
	UInt32 v = 0;

	if (offset < sizeof(kConfig) - 3) {
		memcpy(&v, kConfig + (offset & ~3u), sizeof(v));
	}
	return v;
}

UInt16
BeringiaPCIDevice::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
	UInt16 v = 0;

	if (offset < sizeof(kConfig) - 1) {
		memcpy(&v, kConfig + (offset & ~1u), sizeof(v));
	}
	return v;
}

UInt8
BeringiaPCIDevice::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
	return offset < sizeof(kConfig) ? kConfig[offset] : 0;
}
