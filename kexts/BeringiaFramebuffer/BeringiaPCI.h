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

/* what AppleParavirtGPU matches: PCI 106b:eeee, class 0x030000 (display controller, VGA-compatible) */
#define BERINGIA_PCI_VENDOR     0x106b
#define BERINGIA_PCI_DEVICE     0xeeee
#define BERINGIA_PCI_CLASS      0x030000        /* revision 0, prog-if 0, subclass 0, base class 3 */

class BeringiaPCIDevice : public IOPCIDevice
{
	OSDeclareDefaultStructors(BeringiaPCIDevice)

	static const UInt8 kConfig[64];

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	virtual UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
};

#endif /* _BERINGIA_PCI_H_ */
