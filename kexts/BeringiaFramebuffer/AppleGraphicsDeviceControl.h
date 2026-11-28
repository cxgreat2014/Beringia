// SPDX-License-Identifier: APSL-2.0
/*
 * AppleGraphicsDeviceControl.h - source-level reconstruction of the closed KDK class
 * AppleGraphicsDeviceControl (com.apple.AppleGraphicsDeviceControl 8.1.11, KDK 25A349 /
 * macOS 26.0 25A353, x86_64).  Draft for Beringia: subclass it from a kext.
 *
 * Everything here was derived from the KDK binary, read-only:
 *   .../AppleGraphicsControl.kext/Contents/PlugIns/AppleGraphicsDeviceControl.kext/Contents/MacOS/AppleGraphicsDeviceControl
 * and compared with its subclass AppleParavirtGPUControl
 *   .../AppleParavirtGPU.kext/Contents/MacOS/AppleParavirtGPU  (thin x86_64)
 *
 * Class facts (from the binary, not guessed):
 *   - base class: IOService, directly (C2 ctor calls __ZN9IOServiceC2EPK11OSMetaClass)
 *   - MetaClass alloc size 0x110 (0x110 = sizeof(IOService) 0x88 + 0x88 private ivars)
 *   - vtable __ZTV26AppleGraphicsDeviceControl = __const+0x3d8; address point +0x10
 *     (vptr is __ZTV+0x10, as the ctor stores 0x5488)
 *   - 276 virtual slots; slots 0..265 are IOService/OSObject/IORegistryEntry, 266 is the
 *     first virtual AppleGraphicsDeviceControl declares, 275 the last.
 *
 * Only ONE AppleGraphicsDeviceControl virtual has to be implemented by a concrete subclass:
 * slot 266, the pure virtual vendor_doDeviceAttribute(..., IOExternalMethodArguments *).
 * AppleParavirtGPUControl overrides exactly (vtable diff vs AGDC):
 *   slot  0/1  destructor D1/D0, slot 7 getMetaClass(), slot 18 free(),
 *   slot 184   start(IOService *),
 *   slot 266   vendor_doDeviceAttribute(UInt32, unsigned long *, unsigned long,
 *                                      unsigned long *, unsigned long *, IOExternalMethodArguments *)
 * It does NOT override stop/terminate/message/newUserClient (those vtable entries still
 * point at AppleGraphicsDeviceControl::...); the base newUserClient(type 2) creates an
 * AppleGraphicsDeviceControlClient that funnels every external method into the slot-266 hook.
 *
 * AGDC attribute plumbing (non-virtual, exported):
 *   getDeviceAttribute(sel, out, outSize)  -> filtered_doDeviceAttribute(kGet,  sel, 0, 0, out, outSize, NULL)
 *   setDeviceAttribute(sel, in, value)     -> filtered_doDeviceAttribute(kSet,  sel, in, value, 0, 0, NULL)
 *   doDeviceAttribute (sel, p, v, a, b)    -> filtered_doDeviceAttribute(kDo,   sel, ...)
 *   user client externalMethod             -> UserKernelTransfer(sel, state)
 *                                          -> filtered_doDeviceAttribute(kUserClient, sel, ...)
 * filtered_doDeviceAttribute handles some selectors itself (0x3, 0x10, 0x20, 0x80..,
 * 0x300, 0x301, 0x920.., 0x980.., 0x4100..) and otherwise dispatches through virtual slot 267
 * to vendor_doDeviceAttribute(..., AGDCClientState_t *), which in the base only forwards
 * state->args (state+0x38) to the pure slot-266 override.  So the subclass hook is reached
 * for every selector by both the in-kernel attribute API and the user client.
 *
 * AGDC attribute selector values observed in the binary / used by AppleGPUWrangler and
 * IOPresentment (see notes-gpuwrangler.md, notes-presentment.md, notes-agdc-header.md).
 * In the slot-266 hook, p3/p4 are always the OUTPUT pair (buffer, &size) and p1/p2 the
 * INPUT pair (buffer, size); the same code serves the in-kernel get*() API and the user
 * client, because filtered_doDeviceAttribute maps them identically:
 *
 *   selector 1  kAGDCVendorInfo
 *      GET/in-kernel: getDeviceAttribute(1, &info, &size) with size == 0x2c
 *      USER CLIENT:   IOConnectCallMethod(conn, 1, ...) structure output 0x2c
 *      handler requires *p4 == 0x2c and fills AGDCVendorInfo at p3; iface (p3+0x28) must
 *      be 1 or 2 for IOPresentment, and in {1,2,7} for GPUWrangler (bit test 0x86>>iface).
 *
 *   selector 3  kAGDCFramebufferInfo
 *      GET  (p2 == 0):  if *p4 == 4, write the 32-bit flagsMask to *(u32 *)p3.
 *                       GPUWrangler then requires (value & 3) == 3.
 *      SET  (p2 == 4):  flags = flagsMask & *(u32 *)p1; create framebuffers (bit0) and
 *                       the accelerator (bit1).  Base filtered also touches +0xb5/+0xb6.
 *
 *   selector 0x980 kAGDCGPUCapability
 *      GET/user client: requires *p4 == 0xdc, fills AGDCGPUCapability at p3.  IOPresentment
 *      reads framebufferCount at +0x30 and the IOFramebuffer registry-ID array at +0x3c.
 *
 *   selector 0x920  accelerator / framebuffer enumeration (count at this+0x114,
 *                   registry IDs at this+0x118[] in AppleParavirtGPUControl)
 *   selector 0x921  0xb0-byte display description out (requires *p4 == 0xb0)
 *   selector 0x2001 device info (AppleParavirtGPUControl dispatches it)
 *
 *   setDeviceAttribute(0x301, &gpuId, 2) is consumed by base filtered (not the hook):
 *   it requires the ulong value == 2 and stores the u16 at *(u16 *)in into this+0xb0.
 *
 * AGDCClientState_t (opaque here) is built by AppleGraphicsDeviceControlClient: its
 * +0x38 field is the IOExternalMethodArguments * that the base slot-267 bridge hands to
 * the slot-266 hook (NULL for the in-kernel get/set/do paths).
 */
#ifndef _BERINGIA_APPLE_GRAPHICS_DEVICE_CONTROL_H
#define _BERINGIA_APPLE_GRAPHICS_DEVICE_CONTROL_H

#include <IOKit/IOService.h>

struct IOExternalMethodArguments;   /* IOKit/IOUserClient.h (pointer only here) */
struct AGDCClientState_t;           /* private; +0x38 is IOExternalMethodArguments * */

/* ---- AGDC attribute selectors (vendor_doDeviceAttribute / getDeviceAttribute) ---- */
enum {
	kAGDCVendorInfo             = 1,      /* AGDCVendorInfo        out-only, 0x2c bytes */
	kAGDCFramebufferInfo        = 3,      /* flags in; create FB / accelerator        */
	kAGDCGPUInfo                = 0x920,  /* accelerator / framebuffer enumeration    */
	kAGDCDisplayInfo            = 0x921,  /* 0xb0-byte display description out        */
	kAGDCGPUCapability          = 0x980,  /* AGDCGPUCapability     out-only, 0xdc bytes*/
	kAGDCGPUDeviceInfo          = 0x2001,
	kAGDCSetGPUId               = 0x301,  /* setDeviceAttribute: u16 GPU id, arg 2    */
	kAGDCSetInitFlags           = 3,      /* setDeviceAttribute: init flags           */
	kAGDCInitFlags              = 0x300,  /* getDeviceAttribute: init flags           */

	/* user client type passed to IOServiceOpen() for the AGDC attribute client */
	kAGDCUserClientType         = 2,
};

/* Vendor info returned by getDeviceAttribute(kAGDCVendorInfo,...) / vendor selector 1.
 * Layout written by AppleParavirtGPUControl::vendor_doDeviceAttribute (0x13e6):
 *   +0x00 u32 0x00010000
 *   +0x04 char[16] "AppleParavirt"
 *   +0x14 u64 0
 *   +0x1c u64 0
 *   +0x24 u32 platformID = vendor:device (u16 vendor | u16 device << 16) = 0xEEEE106B
 *   +0x28 u32 iface (1 or 2; IOPresentment accepts 1/2, GPUWrangler 1/2/7)
 * sizeof == 0x2c.
 */
struct __attribute__((packed)) AGDCVendorInfo {
	UInt32 version;        /* +0x00  0x00010000 */
	char   name[16];       /* +0x04  vendor name, NUL padded */
	UInt64 reserved0;      /* +0x14 */
	UInt64 reserved1;      /* +0x1c */
	UInt32 platformID;     /* +0x24  u16 vendor, u16 device */
	UInt32 iface;          /* +0x28  1 or 2 */
};
static_assert(sizeof(AGDCVendorInfo) == 0x2c, "AGDCVendorInfo must be 0x2c bytes");
static_assert(__builtin_offsetof(AGDCVendorInfo, version)    == 0x00, "AGDCVendorInfo.version");
static_assert(__builtin_offsetof(AGDCVendorInfo, name)       == 0x04, "AGDCVendorInfo.name");
static_assert(__builtin_offsetof(AGDCVendorInfo, reserved0)  == 0x14, "AGDCVendorInfo.reserved0");
static_assert(__builtin_offsetof(AGDCVendorInfo, reserved1)  == 0x1c, "AGDCVendorInfo.reserved1");
static_assert(__builtin_offsetof(AGDCVendorInfo, platformID) == 0x24, "AGDCVendorInfo.platformID");
static_assert(__builtin_offsetof(AGDCVendorInfo, iface)      == 0x28, "AGDCVendorInfo.iface");

/* GPU capability table returned by getDeviceAttribute(kAGDCGPUCapability,...) / vendor 0x980.
 * Layout written by AppleParavirtGPUControl (0x1342) and read by IOPresentment
 * (count +0x30, IOFramebuffer registry-ID array +0x3c).  Packed: the binary writes the
 * u64 at +0x34 and the array at +0x3c with no padding.
 * sizeof == 0xdc.
 */
struct __attribute__((packed)) AGDCGPUCapability {
	UInt64 mask[4];          /* +0x00  four u64 capability bitmasks */
	UInt32 count20;          /* +0x20 */
	UInt32 count24;          /* +0x24 */
	UInt32 count28;          /* +0x28 */
	UInt32 count2c;          /* +0x2c */
	UInt32 framebufferCount; /* +0x30  number of valid entries in fbIDs[]  */
	UInt64 providerID;       /* +0x34  (binary stores getProvider() result) */
	UInt64 fbIDs[20];        /* +0x3c  IOFramebuffer registry entry IDs     */
};
static_assert(sizeof(AGDCGPUCapability) == 0xdc, "AGDCGPUCapability must be 0xdc bytes");
static_assert(__builtin_offsetof(AGDCGPUCapability, mask)             == 0x00, "AGDCGPUCapability.mask");
static_assert(__builtin_offsetof(AGDCGPUCapability, count20)          == 0x20, "AGDCGPUCapability.count20");
static_assert(__builtin_offsetof(AGDCGPUCapability, count24)          == 0x24, "AGDCGPUCapability.count24");
static_assert(__builtin_offsetof(AGDCGPUCapability, count28)          == 0x28, "AGDCGPUCapability.count28");
static_assert(__builtin_offsetof(AGDCGPUCapability, count2c)          == 0x2c, "AGDCGPUCapability.count2c");
static_assert(__builtin_offsetof(AGDCGPUCapability, framebufferCount) == 0x30, "AGDCGPUCapability.framebufferCount");
static_assert(__builtin_offsetof(AGDCGPUCapability, providerID)       == 0x34, "AGDCGPUCapability.providerID");
static_assert(__builtin_offsetof(AGDCGPUCapability, fbIDs)            == 0x3c, "AGDCGPUCapability.fbIDs");

class AppleGraphicsDeviceControl : public IOService
{
	OSDeclareAbstractStructors(AppleGraphicsDeviceControl);

public:
	/* ---- IOService virtuals that AppleGraphicsDeviceControl itself overrides ----
	 * These are NOT new virtual slots: they re-declare IOService slots 184 (start),
	 * 185 (stop), 192 (terminate), 230 (message), 238 (newUserClient 5-arg).  Declaring
	 * them here makes an inheriting subclass's vtable point at the AppleGraphicsDeviceControl
	 * implementations (from the AGDC kext) instead of IOService's base versions.  The base
	 * newUserClient(type 2) is the constructor of AppleGraphicsDeviceControlClient. */
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual bool terminate(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	                               OSDictionary *properties,
	                               IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	/* api tag passed to filtered_doDeviceAttribute: 0=get, 1=set, 2=do, 3=user client */
	enum agdc_filtered_api_t {
		kAGDCFilteredGet        = 0,
		kAGDCFilteredSet        = 1,
		kAGDCFilteredDo         = 2,
		kAGDCFilteredUserClient = 3,
	};

	/* ---- in-kernel attribute API (non-virtual, exported) ----
	 * "out"/"in" are unsigned long * on LP64 (mangled ...EjPmS0_ / EjPmm), i.e. the
	 * selector-specific payload pointer; outSize/inSize is a size_t value (unsigned long)
	 * or pointer depending on the call, exactly as the mangled signature says. */
	IOReturn getDeviceAttribute(unsigned int selector, unsigned long *out,
	                            unsigned long *outSize);
	IOReturn setDeviceAttribute(unsigned int selector, unsigned long *in,
	                            unsigned long value);
	IOReturn doDeviceAttribute(unsigned int selector, unsigned long *a, unsigned long b,
	                           unsigned long *c, unsigned long *d);

	/* user client path: externalMethod -> UserKernelTransfer -> filtered_doDeviceAttribute */
	IOReturn UserKernelTransfer(unsigned int selector, AGDCClientState_t *state);
	IOReturn filtered_doDeviceAttribute(agdc_filtered_api_t api, unsigned int selector,
	                                    unsigned long *p1, unsigned long p2,
	                                    unsigned long *p3, unsigned long *p4,
	                                    AGDCClientState_t *state);

	/* ===================== virtuals (slots 266..275 after IOService 0..265) ========= */
	/* slot 266: the ONLY pure virtual - a concrete subclass must implement this.
	 * Called for every selector not consumed by filtered_doDeviceAttribute; args is the
	 * IOExternalMethodArguments * (user client) or NULL (in-kernel get/set/do calls). */
	virtual IOReturn vendor_doDeviceAttribute(unsigned int selector,
	                                          unsigned long *p1, unsigned long p2,
	                                          unsigned long *p3, unsigned long *p4,
	                                          IOExternalMethodArguments *args) = 0;

	/* slot 267: base bridge; forwards state->args to slot 266. Not normally overridden. */
	virtual IOReturn vendor_doDeviceAttribute(unsigned int selector,
	                                          unsigned long *p1, unsigned long p2,
	                                          unsigned long *p3, unsigned long *p4,
	                                          AGDCClientState_t *state);

	/* slots 268..275: reserved; inherit AppleGraphicsDeviceControl stubs. */
	virtual void _RESERVEDAppleGraphicsDeviceControl0();
	virtual void _RESERVEDAppleGraphicsDeviceControl1();
	virtual void _RESERVEDAppleGraphicsDeviceControl2();
	virtual void _RESERVEDAppleGraphicsDeviceControl3();
	virtual void _RESERVEDAppleGraphicsDeviceControl4();
	virtual void _RESERVEDAppleGraphicsDeviceControl5();
	virtual void _RESERVEDAppleGraphicsDeviceControl6();
	virtual void _RESERVEDAppleGraphicsDeviceControl7();

private:
	/* Real AppleGraphicsDeviceControl ivars (not declared by any public header).  We only
	 * need the correct total SIZE (0x110) so that a subclass own members do not overlap
	 * them; the base methods access these by absolute offset.  Known offsets:
	 *   +0xa8 OSObject *   (checked in the plugin dispatch)
	 *   +0xb0 UInt16 gpuId (setDeviceAttribute(0x301,...))
	 *   +0xb4 UInt8 flags, +0xb5 UInt8, +0xb6 UInt8 (HDA property publishing)
	 */
	UInt8  _reserved0[0x20];   /* 0x88..0xa7 */
	void  *_fieldA8;           /* 0xa8 */
	UInt16 _gpuId;             /* 0xb0 */
	UInt16 _padB2;             /* 0xb2 */
	UInt8  _flagsB4;           /* 0xb4 */
	UInt8  _hdaFlagsB5;        /* 0xb5 */
	UInt8  _hdaPublishedB6;    /* 0xb6 */
	UInt8  _padB7;             /* 0xb7 */
	UInt8  _reserved1[0x58];   /* 0xb8..0x10f */
};
static_assert(sizeof(AppleGraphicsDeviceControl) == 0x110,
              "AppleGraphicsDeviceControl instance size must be 0x110");

#endif /* _BERINGIA_APPLE_GRAPHICS_DEVICE_CONTROL_H */
