// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaDisplayHost: the service that claims the device-tree node /display and builds the virtual GPU under it.
 *
 * It is deliberately not an IOFramebuffer: the node's framebuffer is the one under the virtual PCI device, and an
 * IOFramebuffer whose start() declined the node would hand it to the next driver in the IOMatchCategory (IONDRVSupport's
 * IONDRVFramebuffer), which then drove the same VRAM as a second GPU (run 20261128-065909). With beringia_pci off this
 * service does nothing and the node stays for whatever else matches it.
 */
#ifndef _BERINGIA_DISPLAY_HOST_H_
#define _BERINGIA_DISPLAY_HOST_H_

#include <IOKit/IOService.h>
#include "BeringiaPCI.h"
#include "BeringiaGPUControl.h"

class BeringiaDisplayHost : public IOService
{
	OSDeclareDefaultStructors(BeringiaDisplayHost)

	BeringiaPCIBridge *fPCIBridge;
	BeringiaPCIDevice *fPCI;
	BeringiaGPUControl *fGPUControl;

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

#endif /* _BERINGIA_DISPLAY_HOST_H_ */
