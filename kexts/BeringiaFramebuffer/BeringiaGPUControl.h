// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaGPUControl: the AppleGraphicsDeviceControl service of the virtual GPU.
 *
 * GPUWrangler identifies an AGDC by class and stores its registry ID in the GPU record (`agdc`); IOPresentment opens
 * its user client (type 2), asks kAGDCVendorInfo (selector 1) for the platform identity and kAGDCGPUCapability
 * (selector 0x980) for the list of framebuffers, and only then builds the device map that CoreDisplay's DisplayPipe
 * needs. The base class consumes the generic selectors; ours answers the vendor ones. See
 * AppleGraphicsDeviceControl.h (reconstructed from the KDK binary) for the layouts and the attribute protocol.
 */
#ifndef _BERINGIA_GPU_CONTROL_H_
#define _BERINGIA_GPU_CONTROL_H_

#include "AppleGraphicsDeviceControl.h"

class BeringiaGPUControl : public AppleGraphicsDeviceControl
{
	OSDeclareDefaultStructors(BeringiaGPUControl)

	/* the framebuffer service itself: the base class turns the objects in AGDCGPUCapability into the registry IDs
	 * the user client (IOPresentment) reads */
	IOService *fFramebuffer;

public:
	void setFramebuffer(IOService *framebuffer);

	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn vendor_doDeviceAttribute(unsigned int selector, unsigned long *p1, unsigned long p2,
	    unsigned long *p3, unsigned long *p4, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};

#endif /* _BERINGIA_GPU_CONTROL_H_ */
