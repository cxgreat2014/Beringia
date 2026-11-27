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
OSDefineMetaClassAndStructors(BeringiaPCIBridge, IOPCIBridge);

/* a 64-byte configuration space, little-endian dwords as a bridge would return them */
static const UInt8
kBeringiaPCIConfig[64] = {
	0x6b, 0x10, 0xe1, 0x0b,         /* 0x00 vendor 0x106b, device 0x0be1 */
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
	0x6b, 0x10, 0xe1, 0x0b,         /* 0x2c subsystem vendor 0x106b, subsystem id 0x0be1 */
	0x00, 0x00, 0x00, 0x00,         /* 0x30 ROM BAR */
	0x00, 0x00, 0x00, 0x00,         /* 0x34 capability pointer */
	0x00, 0x00, 0x00, 0x00,         /* 0x38 */
	0x00, 0x01, 0x00, 0x00,         /* 0x3c interrupt line, interrupt pin 1 (INTA) */
};

/* Apple's IOPCIConfigurator publishes PCI properties as 4-byte OSData (IOPCIConfigurator::constructProperties):
 * vendor-id is the low half of config dword 0, class-code is config dword 8 >> 8. GPUWrangler casts class-code to
 * OSData and reads bytes 2..3, so an OSNumber would not do. */
static void
setData(IOService *service, const char *key, UInt32 value)
{
	UInt8 bytes[4] = { (UInt8)value, (UInt8)(value >> 8), (UInt8)(value >> 16), (UInt8)(value >> 24) };
	OSData *d = OSData::withBytes(bytes, sizeof(bytes));

	if (d) {
		service->setProperty(key, d);
		d->release();
	}
}

static UInt8
configByte(UInt8 offset)
{
	return offset < sizeof(kBeringiaPCIConfig) ? kBeringiaPCIConfig[offset] : 0;
}

bool
BeringiaPCIDevice::start(IOService *provider)
{
	/* IOPCIDevice::start would ask the provider for a real host bridge; publish what IOPCIConfigurator would */
	setData(this, "vendor-id", BERINGIA_PCI_VENDOR);
	setData(this, "device-id", BERINGIA_PCI_DEVICE);
	setData(this, "class-code", BERINGIA_PCI_CLASS >> 8);   /* <00 00 03 00> */
	setData(this, "revision-id", 0);
	setData(this, "subsystem-vendor-id", BERINGIA_PCI_VENDOR);
	setData(this, "subsystem-id", BERINGIA_PCI_DEVICE);
	setName("BeringiaGPU");
	return true;
}

void
BeringiaPCIDevice::setPCIBridge(IOPCIBridge *bridge)
{
	parent = bridge;        /* protected: IOService::attach does not set it, and configAccess dereferences it */
}

UInt32
BeringiaPCIDevice::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
	UInt32 v = 0;
	UInt8 i;

	if (offset >= sizeof(kBeringiaPCIConfig) - 3) {
		return 0;
	}
	for (i = 0; i < 4; i++) {
		v |= (UInt32)configByte((UInt8)((offset & ~3u) + i)) << (8 * i);
	}
	return v;
}

UInt16
BeringiaPCIDevice::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
	UInt16 v = 0;

	if (offset >= sizeof(kBeringiaPCIConfig) - 1) {
		return 0;
	}
	v = (UInt16)configByte((UInt8)(offset & ~1u)) | ((UInt16)configByte((UInt8)((offset & ~1u) + 1)) << 8);
	return v;
}

UInt8
BeringiaPCIDevice::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
	return configByte(offset);
}

/*
 * The bridge: it exists so the device has an IOPCIBridge provider and ExpansionData (GPUWrangler and IOPCIDevice's
 * configAccess dereference both). It enumerates nothing; the one device under it was created directly.
 */
IODeviceMemory *
BeringiaPCIBridge::ioDeviceMemory(void)
{
	return NULL;
}

UInt32
BeringiaPCIBridge::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
	UInt32 v = 0;
	UInt8 i;

	if (offset >= sizeof(kBeringiaPCIConfig) - 3) {
		return 0xffffffff;
	}
	for (i = 0; i < 4; i++) {
		v |= (UInt32)configByte((UInt8)((offset & ~3u) + i)) << (8 * i);
	}
	return v;
}

void
BeringiaPCIBridge::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
}

UInt16
BeringiaPCIBridge::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
	UInt16 v = 0;

	if (offset >= sizeof(kBeringiaPCIConfig) - 1) {
		return 0xffff;
	}
	v = (UInt16)configByte((UInt8)(offset & ~1u)) | ((UInt16)configByte((UInt8)((offset & ~1u) + 1)) << 8);
	return v;
}

void
BeringiaPCIBridge::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
}

UInt8
BeringiaPCIBridge::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
	return configByte(offset);
}

void
BeringiaPCIBridge::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
}

IOPCIAddressSpace
BeringiaPCIBridge::getBridgeSpace(void)
{
	IOPCIAddressSpace space;

	space.bits = 0;         /* bus 0, device 0, function 0 */
	return space;
}

IOReturn
BeringiaPCIBridge::setLinkSpeed(tIOPCILinkSpeed linkSpeed, bool retrain)
{
	return kIOReturnUnsupported;
}

IOReturn
BeringiaPCIBridge::getLinkSpeed(tIOPCILinkSpeed *linkSpeed)
{
	if (linkSpeed) {
		*linkSpeed = kIOPCILinkSpeed_2_5_GTs;   /* there is no lower value, and no link at all */
	}
	return kIOReturnUnsupported;
}
