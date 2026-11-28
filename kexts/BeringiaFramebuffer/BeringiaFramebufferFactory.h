// SPDX-License-Identifier: APSL-2.0
/* The framebuffer class itself lives in BeringiaFramebuffer.cpp (it is never matched by a personality: BeringiaDisplayHost
 * creates it under the virtual PCI device). This is the only interface BeringiaDisplayHost needs. */
#ifndef _BERINGIA_FRAMEBUFFER_FACTORY_H_
#define _BERINGIA_FRAMEBUFFER_FACTORY_H_

class IOService;

/* a new, initialised BeringiaFramebuffer (not yet attached or started); NULL on allocation failure */
IOService *beringia_framebuffer_create(void);

#endif /* _BERINGIA_FRAMEBUFFER_FACTORY_H_ */
