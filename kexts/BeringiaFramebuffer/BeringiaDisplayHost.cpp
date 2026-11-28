// SPDX-License-Identifier: APSL-2.0
/*
 * Builds, under the device-tree node: the virtual PCI bridge and device (GPUWrangler lists a GPU only with an
 * IOPCIDevice ancestor), the AGDC service (IOPresentment's device map) and the framebuffer under the device. See
 * BeringiaPCI.h, BeringiaGPUControl.h and README.md.
 */
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "BeringiaDisplayHost.h"
#include "BeringiaFramebufferFactory.h"

OSDefineMetaClassAndStructors(BeringiaDisplayHost, IOService);

bool
BeringiaDisplayHost::start(IOService *provider)
{
	IOService *fb;

	/* the virtual GPU is an experiment: boot with beringia_pci=1 to build it */
	uint32_t pci = 0;
	if (!PE_parse_boot_argn("beringia_pci", &pci, sizeof(pci)) || !pci) {
		return false;
	}
	/*
	 * The device must be an ordinary IOPCIDevice with an IOPCIBridge provider: GPUWrangler stores
	 * safeMetaCast(device->getProvider(), IOPCIBridge) and dereferences it, and IOPCIDevice's configAccess
	 * dereferences the bridge's ExpansionData. Neither is registered or probed; the bridge enumerates nothing and the
	 * device is created here and published once.
	 */
	fPCIBridge = new BeringiaPCIBridge;
	if (!fPCIBridge || !fPCIBridge->init(NULL) || !fPCIBridge->IOService::attach(provider)) {
		IOLog("BeringiaDisplayHost: no PCI bridge under %s\n", provider->getName());
		return false;
	}
	fPCI = new BeringiaPCIDevice;
	if (!fPCI || !fPCI->init(NULL)) {
		IOLog("BeringiaDisplayHost: no virtual PCI device\n");
		return false;
	}
	fPCI->setPCIBridge(fPCIBridge);
	fPCI->space.bits = 0;           /* bus 0, device 0, function 0 */
	if (!fPCI->IOService::attach(fPCIBridge) || !fPCI->start(fPCIBridge)) {
		IOLog("BeringiaDisplayHost: the PCI device did not start\n");
		return false;
	}
	IOLog("BeringiaDisplayHost: PCI %04x:%04x config %08x class %08x\n", BERINGIA_PCI_VENDOR, BERINGIA_PCI_DEVICE,
	    fPCI->extendedConfigRead32(0), fPCI->extendedConfigRead32(8));
	fPCI->registerService(0);

	/* the AGDC service: GPUWrangler keeps its registry ID and IOPresentment asks it for the framebuffer list */
	fGPUControl = new BeringiaGPUControl;
	if (!fGPUControl || !fGPUControl->init(NULL) || !fGPUControl->IOService::attach(fPCI) ||
	    !fGPUControl->start(fPCI)) {
		IOLog("BeringiaDisplayHost: the AGDC service did not start\n");
		return false;
	}
	fGPUControl->registerService(0);

	fb = beringia_framebuffer_create();
	if (!fb) {
		return false;
	}
	if (!fb->attach(fPCI) || !fb->start(fPCI)) {
		IOLog("BeringiaDisplayHost: the framebuffer did not start\n");
		fb->release();
		return false;
	}
	fb->registerService();
	fb->release();                  /* the provider's child link holds it now */
	fGPUControl->setFramebuffer(fb);
	IOLog("BeringiaDisplayHost: hosted under virtual PCI %04x:%04x\n", BERINGIA_PCI_VENDOR, BERINGIA_PCI_DEVICE);
	return true;
}

void
BeringiaDisplayHost::free(void)
{
	OSSafeReleaseNULL(fGPUControl);     /* before the device it hangs from */
	OSSafeReleaseNULL(fPCI);
	OSSafeReleaseNULL(fPCIBridge);
	IOService::free();
}
