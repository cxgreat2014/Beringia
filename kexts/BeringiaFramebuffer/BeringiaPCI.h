// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaPCIDevice: the virtual PCI display device BeringiaFramebuffer attaches to.
 *
 * GPUWrangler lists a GPU only when an IOFramebuffer or IOAccelerator has an IOPCIDevice ancestor; Apple's own software
 * GPU (AppleParavirtGPU, 106B:EEEE) is a fake PCI device, and IOPresentment's device map needs a PCI display-class
 * ancestor. Its config space is synthesized here: a real device's reads go through an IOPCIBridge, which this machine
 * has none of, so start() publishes the properties itself and the configRead* overrides answer from a table.
 */
#ifndef _BERINGIA_PCI_H_
#define _BERINGIA_PCI_H_

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>

/* A display-class PCI device of our own: 106b:0be1, class 0x030000. It is deliberately not 106b:eeee,
 * AppleParavirtGPU's id: that driver matches it, and its getResources() faults on a device without real BARs
 * (run 20261128-064951), while nothing else needs the paravirt id. */
#define BERINGIA_PCI_VENDOR     0x106b
#define BERINGIA_PCI_DEVICE     0x0be1
#define BERINGIA_PCI_CLASS      0x030000        /* revision 0, prog-if 0, subclass 0, base class 3 */

/* The bridge the device hangs from: GPUWrangler stores safeMetaCast(device->getProvider(), IOPCIBridge) and
 * dereferences it unguarded, and IOPCIDevice's config reads dereference the bridge's ExpansionData. It vends no nubs
 * of its own; BeringiaFramebuffer creates the one device and sets its protected parent. */
class BeringiaPCIBridge : public IOPCIBridge
{
	OSDeclareDefaultStructors(BeringiaPCIBridge)

public:
	virtual IODeviceMemory *ioDeviceMemory(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual void configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) APPLE_KEXT_OVERRIDE;
	virtual UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual void configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) APPLE_KEXT_OVERRIDE;
	virtual UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual void configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) APPLE_KEXT_OVERRIDE;
	virtual IOPCIAddressSpace getBridgeSpace(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setLinkSpeed(tIOPCILinkSpeed linkSpeed, bool retrain) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getLinkSpeed(tIOPCILinkSpeed *linkSpeed) APPLE_KEXT_OVERRIDE;
	/* The base class matches and probes through host-bridge state this device does not have: it walks config space
	 * of a real bus (matchKeys, getNubResources) and scans buses (probeBus, configure). Nothing here enumerates or
	 * matches drivers; the one device is created and published by BeringiaFramebuffer. */
	virtual bool matchNubWithPropertyTable(IOService *nub, OSDictionary *propertyTable, SInt32 *score) APPLE_KEXT_OVERRIDE;
	virtual bool pciMatchNub(IOPCIDevice *nub, OSDictionary *table, SInt32 *score) APPLE_KEXT_OVERRIDE;
	virtual bool matchKeys(IOPCIDevice *nub, const char *keys, UInt32 defaultMask, UInt8 regNum) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getNubResources(IOService *nub) APPLE_KEXT_OVERRIDE;
	virtual void probeBus(IOService *provider, UInt8 busNum) APPLE_KEXT_OVERRIDE;
	virtual bool configure(IOService *provider) APPLE_KEXT_OVERRIDE;
};

class BeringiaPCIDevice : public IOPCIDevice
{
	OSDeclareDefaultStructors(BeringiaPCIDevice)

public:
	/* IOPCIDevice::parent is protected; IOService::attach does not set it and configAccess dereferences it */
	void setPCIBridge(IOPCIBridge *bridge);

	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	/* no BARs to assign; IOPCIDevice::getResources() walks resource state only a real host bridge sets up */
	virtual IOReturn getResources(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
};

#endif /* _BERINGIA_PCI_H_ */
