// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaFramebuffer: an IOFramebuffer over the boot framebuffer the Beringia loader provides (loader fb=WxH).
 *
 * The memory is the device-tree node /display's "reg" range, outside XNU's RAM; its geometry is the boot console's
 * (boot_args Video, through PE_Video). Linux shows the same memory (/dev/beringia-fb). There is one mode, at the
 * boot geometry, 32 bpp, which this driver describes completely, unlike IONDRVSupport's IOBootNDRV: a 60 Hz detailed
 * timing and an EDID, so that CoreDisplay can build a mode list and a display identity (with IOBootNDRV it found no
 * usable mode, kept the display offline and composited into a virtual framebuffer).
 *
 * No hardware: no cursor, no gamma, no vertical-blank interrupt (WindowServer times its frames itself). The first
 * calls of each entry point are logged ("BeringiaFramebuffer: ..."), to see what WindowServer asks for.
 *
 * The display (IODisplayConnect, then AppleDisplay) attaches only once WindowServer opens the framebuffer, after
 * CoreDisplay has looked for it and found none: CoreDisplay then marks the connection unusable, builds no mode list
 * and composites into a virtual framebuffer. So the driver supports the connect-change interrupt and raises it once,
 * shortly after an IODisplay appears under it, as a monitor being plugged in would; CoreDisplay then rebuilds the
 * connection with the display present.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "BeringiaPCI.h"
#include "BeringiaFramebufferFactory.h"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

#define TRACE(fmt, ...) do { \
	static int traced; \
	if (traced < 4) { \
		traced++; \
		IOLog("BeringiaFramebuffer: " fmt "\n", ##__VA_ARGS__); \
	} \
} while (0)

enum {
	kMode = 1,                 /* the only display mode */
	kRefreshHz = 60,
	kHBlank = 160,             /* CVT reduced blanking: front porch 48, sync 32, back porch 80 */
	kHFront = 48,
	kHSync = 32,
	kVFront = 3,
	kVSync = 6,
};

class BeringiaFramebuffer : public IOFramebuffer
{
	OSDeclareDefaultStructors(BeringiaFramebuffer);

	IODeviceMemory *fVRAM;     /* the provider's range 0: the whole reserved framebuffer region */
	UInt32 fWidth, fHeight, fStride;
	UInt32 fVBlank;
	UInt8 fEDID[128];
	/* the connect-change interrupt IOFramebuffer registers ('dci ') */
	IOFBInterruptProc fConnectProc;
	OSObject *fConnectTarget;
	void *fConnectRef;
	bool fConnectRaised;
	IONotifier *fDisplayNotifier;
	IOTimerEventSource *fConnectTimer;
	thread_call_t fOpenCall;
	bool fStopping;

	UInt64 pixelClock(void) const
	{
		return (UInt64)(fWidth + kHBlank) * (fHeight + fVBlank) * kRefreshHz;
	}
	/* EDID's detailed timing has 12-bit active fields and a 16-bit 10 kHz pixel clock: a larger mode cannot be
	 * described (4096 would wrap to 0), so start() refuses it rather than publish a mode that is not the memory. */
	bool edidFits(void) const
	{
		return fWidth <= 4095 && fHeight <= 4095 && pixelClock() / 10000 <= 0xffff;
	}
	void buildEDID(void);
	static bool displayAppeared(void *target, void *refCon, IOService *display, IONotifier *notifier);
	static void raiseConnectChange(OSObject *owner, IOTimerEventSource *sender);
	static void earlyOpen(thread_call_param_t self, thread_call_param_t unused);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE;

	virtual IOReturn enableController(void) APPLE_KEXT_OVERRIDE;
	virtual bool isConsoleDevice(void) APPLE_KEXT_OVERRIDE;
	virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
	virtual const char *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
	virtual IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
	virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
	    IOPixelInformation *pixelInfo) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) APPLE_KEXT_OVERRIDE;
	virtual IOItemCount getConnectionCount(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;
	virtual bool hasDDCConnect(IOIndex connect) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType, IOOptionBits options,
	    UInt8 *data, IOByteCount *length) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries,
	    IOOptionBits options) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, OSObject *target,
	    void *ref, void **interruptRef) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BeringiaFramebuffer, IOFramebuffer);

/* A 128-byte EDID 1.3: one detailed timing (the mode), a monitor name, the range limits. */
void
BeringiaFramebuffer::buildEDID(void)
{
	UInt8 *e = fEDID, sum = 0;
	UInt8 *d;
	UInt64 clk = pixelClock() / 10000;      /* EDID unit: 10 kHz */
	UInt32 hmm = fWidth * 264 / 1000, vmm = fHeight * 264 / 1000;  /* ~96 dpi */
	UInt32 i;
	/* "BRN": letters A..Z as 1..26, 5 bits each, big-endian */
	UInt16 mfg = (UInt16)((('B' - 64) << 10) | (('R' - 64) << 5) | ('N' - 64));
	static const char name[] = "Beringia";

	bzero(e, sizeof(fEDID));
	e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xff;     /* header 00 ff*6 00 */
	e[8] = (UInt8)(mfg >> 8);
	e[9] = (UInt8)mfg;
	e[10] = 0x01;                           /* product 0x0001 */
	e[12] = 0x01;                           /* serial 1 */
	e[16] = 1;                              /* week 1 of 2026 */
	e[17] = 2026 - 1990;
	e[18] = 1;                              /* EDID 1.3 */
	e[19] = 3;
	e[20] = 0x80;                           /* digital input */
	e[21] = (UInt8)((hmm + 5) / 10);        /* cm */
	e[22] = (UInt8)((vmm + 5) / 10);
	e[23] = 120;                            /* gamma 2.2 */
	e[24] = 0x0a;                           /* RGB colour, first detailed timing is preferred */
	/* chromaticity: sRGB primaries and D65 */
	static const UInt8 chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
	memcpy(e + 25, chroma, sizeof(chroma));
	for (i = 38; i < 54; i += 2) {          /* no standard timings */
		e[i] = 0x01;
		e[i + 1] = 0x01;
	}
	d = e + 54;                             /* detailed timing descriptor */
	d[0] = (UInt8)clk;
	d[1] = (UInt8)(clk >> 8);
	d[2] = (UInt8)fWidth;
	d[3] = (UInt8)kHBlank;
	d[4] = (UInt8)(((fWidth >> 8) & 0xf) << 4 | ((kHBlank >> 8) & 0xf));
	d[5] = (UInt8)fHeight;
	d[6] = (UInt8)fVBlank;
	d[7] = (UInt8)(((fHeight >> 8) & 0xf) << 4 | ((fVBlank >> 8) & 0xf));
	d[8] = kHFront;
	d[9] = kHSync;
	d[10] = (UInt8)((kVFront & 0xf) << 4 | (kVSync & 0xf));
	d[11] = 0;
	d[12] = (UInt8)hmm;
	d[13] = (UInt8)vmm;
	d[14] = (UInt8)(((hmm >> 8) & 0xf) << 4 | ((vmm >> 8) & 0xf));
	d[17] = 0x1a;                           /* digital separate sync, hsync +, vsync - (CVT-RB) */
	d = e + 72;                             /* monitor name */
	d[3] = 0xfc;
	for (i = 0; i < 13; i++) {
		d[5 + i] = i < sizeof(name) - 1 ? (UInt8)name[i] : (i == sizeof(name) - 1 ? 0x0a : 0x20);
	}
	d = e + 90;                             /* range limits, around this mode: no display may be told they exclude it */
	d[3] = 0xfd;
	d[5] = 50;                              /* minimum vertical, Hz */
	d[6] = kRefreshHz + 10;
	d[7] = 30;                              /* minimum horizontal, kHz */
	d[8] = (UInt8)min(255u, (unsigned)(((UInt64)(fWidth + kHBlank) * (kRefreshHz + 10) + 999) / 1000));
	d[9] = (UInt8)((clk / 1000 + 1) & 0xff); /* max pixel clock, 10 MHz units */
	d[10] = 0x00;
	d[11] = 0x0a;
	for (i = 12; i < 18; i++) {
		d[i] = 0x20;
	}
	d = e + 108;                            /* unused descriptor */
	d[3] = 0x10;
	e[126] = 0;                             /* no extensions */
	for (i = 0; i < 127; i++) {
		sum += e[i];
	}
	e[127] = (UInt8)(0x100 - sum);
}

/*
 * The instance that matches the device tree node does not drive the display itself: it creates the virtual PCI device
 * under the node and a framebuffer under that, because GPUWrangler lists a GPU only with an IOPCIDevice ancestor (see
 * BeringiaPCI.h). The instance under the PCI device (fHosted) does the work.
 */
bool
BeringiaFramebuffer::start(IOService *provider)
{
	PE_Video video;
	IODeviceMemory *mem;

	bzero(&video, sizeof(video));
	if (!getPlatform() || getPlatform()->getConsoleInfo(&video) != kIOReturnSuccess || !video.v_baseAddr ||
	    video.v_depth != 32 || !video.v_width || !video.v_height || video.v_rowBytes < video.v_width * 4) {
		IOLog("BeringiaFramebuffer: no 32 bpp boot framebuffer (base %#lx depth %lu)\n",
		    (unsigned long)video.v_baseAddr, (unsigned long)video.v_depth);
		return false;
	}
	fWidth = (UInt32)video.v_width;
	fHeight = (UInt32)video.v_height;
	fStride = (UInt32)video.v_rowBytes;
	/* vertical blanking of at least 460 us (CVT-RB): lines / (total lines * refresh) >= 460e-6 s */
	fVBlank = 23;
	while ((UInt64)fVBlank * 1000000ULL < 460ULL * (fHeight + fVBlank) * kRefreshHz) {
		fVBlank++;
	}
	/* provider memory: the device-tree node describes the region the loader reserved; a virtual PCI device has no
	 * bars, and the loader's PE_Video already names the same memory */
	mem = provider->getDeviceMemoryWithIndex(0);
	if (mem) {
		if (video.v_baseAddr < mem->getPhysicalAddress() ||
		    video.v_baseAddr + (UInt64)video.v_rowBytes * video.v_height >
		    mem->getPhysicalAddress() + mem->getLength()) {
			IOLog("BeringiaFramebuffer: the boot framebuffer is not inside the provider's memory\n");
			return false;
		}
		fVRAM = IODeviceMemory::withSubRange(mem, video.v_baseAddr - mem->getPhysicalAddress(),
		    (IOByteCount)fStride * fHeight);
	} else if (OSDynamicCast(IOPCIDevice, provider) != NULL) {
		fVRAM = IODeviceMemory::withRange(video.v_baseAddr, (IOByteCount)fStride * fHeight);
	} else {
		IOLog("BeringiaFramebuffer: the provider has no memory for the boot framebuffer\n");
		return false;
	}
	if (!fVRAM) {
		return false;
	}
	if (!edidFits()) {
		IOLog("BeringiaFramebuffer: %ux%u at %llu Hz has no EDID detailed timing (12-bit sizes, 16-bit clock)\n",
		    fWidth, fHeight, (unsigned long long)pixelClock());
		return false;
	}
	buildEDID();
	IOLog("BeringiaFramebuffer: %ux%u, %u bytes per row, at %#llx; %u lines of vertical blanking, %llu Hz pixel clock\n",
	    fWidth, fHeight, fStride, (unsigned long long)fVRAM->getPhysicalAddress(), fVBlank,
	    (unsigned long long)pixelClock());
	if (!IOFramebuffer::start(provider)) {
		return false;
	}
	/*
	 * Open the framebuffer before WindowServer does, as the console opens a GPU's at boot: opening creates the
	 * display connection, on which AppleDisplay attaches; CoreDisplay considers a framebuffer without a display
	 * offline. From a thread call, not from start (open takes the graphics system gate).
	 */
	fOpenCall = thread_call_allocate(&BeringiaFramebuffer::earlyOpen, this);
	if (fOpenCall) {
		uint64_t deadline;

		retain();
		clock_interval_to_deadline(2, kSecondScale, &deadline);
		if (thread_call_enter_delayed(fOpenCall, deadline)) {
			release();      /* already pending: cannot happen for a fresh call */
		}
	}
	return true;
}

void
BeringiaFramebuffer::earlyOpen(thread_call_param_t param, thread_call_param_t unused)
{
	BeringiaFramebuffer *self = (BeringiaFramebuffer *)param;
	IOReturn r;

	if (self->fStopping) {
		self->release();        /* stopped: do not open a dead framebuffer */
		return;
	}
	r = self->open();
	IOLog("BeringiaFramebuffer: opened at boot: %#x\n", r);
	self->release();
}

void
BeringiaFramebuffer::stop(IOService *provider)
{
	fStopping = true;
	if (fOpenCall && thread_call_cancel(fOpenCall)) {
		release();              /* the call was pending: drop the reference start() took; otherwise earlyOpen does */
	}
	if (fDisplayNotifier) {
		fDisplayNotifier->remove();
		fDisplayNotifier = NULL;
	}
	if (fConnectTimer) {
		fConnectTimer->cancelTimeout();
		if (getWorkLoop()) {
			getWorkLoop()->removeEventSource(fConnectTimer);
		}
		OSSafeReleaseNULL(fConnectTimer);
	}
	IOFramebuffer::stop(provider);
}

void
BeringiaFramebuffer::free(void)
{
	if (fOpenCall) {
		thread_call_free(fOpenCall);
		fOpenCall = NULL;
	}
	OSSafeReleaseNULL(fVRAM);
	IOFramebuffer::free();
}

/* An IODisplay was matched; if it is ours (IODisplay -> IODisplayConnect -> this), raise the interrupt soon. */
bool
BeringiaFramebuffer::displayAppeared(void *target, void *refCon, IOService *display, IONotifier *notifier)
{
	BeringiaFramebuffer *self = (BeringiaFramebuffer *)target;
	IOService *connect = display ? display->getProvider() : NULL;

	if (connect && connect->getProvider() == self && self->fConnectTimer && !self->fConnectRaised) {
		IOLog("BeringiaFramebuffer: display %s attached\n", display->getName());
		self->fConnectTimer->setTimeoutMS(500);
	}
	return true;
}

/* On the work loop: tell IOFramebuffer (and through it WindowServer) that the connection changed, once. */
void
BeringiaFramebuffer::raiseConnectChange(OSObject *owner, IOTimerEventSource *sender)
{
	BeringiaFramebuffer *self = (BeringiaFramebuffer *)owner;

	if (self->fConnectRaised || !self->fConnectProc) {
		return;
	}
	self->fConnectRaised = true;
	IOLog("BeringiaFramebuffer: raising the connect-change interrupt\n");
	self->fConnectProc(self->fConnectTarget, self->fConnectRef);
}

IOReturn
BeringiaFramebuffer::enableController(void)
{
	TRACE("enableController");
	return kIOReturnSuccess;
}

bool
BeringiaFramebuffer::isConsoleDevice(void)
{
	return true;
}

IODeviceMemory *
BeringiaFramebuffer::getApertureRange(IOPixelAperture aperture)
{
	TRACE("getApertureRange(%d)", (int)aperture);
	if (aperture != kIOFBSystemAperture || !fVRAM) {
		return NULL;
	}
	fVRAM->retain();
	return fVRAM;
}

const char *
BeringiaFramebuffer::getPixelFormats(void)
{
	return IO32BitDirectPixels "\0";
}

IOItemCount
BeringiaFramebuffer::getDisplayModeCount(void)
{
	TRACE("getDisplayModeCount");
	return 1;
}

IOReturn
BeringiaFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
	TRACE("getDisplayModes");
	allDisplayModes[0] = kMode;
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
	TRACE("getInformationForDisplayMode(%d)", (int)mode);
	if (mode != kMode || !info) {
		return kIOReturnBadArgument;
	}
	bzero(info, sizeof(*info));
	info->nominalWidth = fWidth;
	info->nominalHeight = fHeight;
	info->refreshRate = kRefreshHz << 16;
	info->maxDepthIndex = 0;
	info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
	return kIOReturnSuccess;
}

UInt64
BeringiaFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth)
{
	return 0;       /* obsolete; the formats come from getPixelInformation */
}

IOReturn
BeringiaFramebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
    IOPixelInformation *pixelInfo)
{
	TRACE("getPixelInformation(%d, %d, %d)", (int)mode, (int)depth, (int)aperture);
	if (mode != kMode || depth != 0 || aperture != kIOFBSystemAperture || !pixelInfo) {
		return kIOReturnBadArgument;
	}
	bzero(pixelInfo, sizeof(*pixelInfo));
	pixelInfo->bytesPerRow = fStride;
	pixelInfo->bytesPerPlane = 0;
	pixelInfo->bitsPerPixel = 32;
	pixelInfo->pixelType = kIORGBDirectPixels;
	pixelInfo->componentCount = 3;
	pixelInfo->bitsPerComponent = 8;
	pixelInfo->componentMasks[0] = 0x00ff0000;
	pixelInfo->componentMasks[1] = 0x0000ff00;
	pixelInfo->componentMasks[2] = 0x000000ff;
	strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
	pixelInfo->activeWidth = fWidth;
	pixelInfo->activeHeight = fHeight;
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
	TRACE("getCurrentDisplayMode");
	if (mode) {
		*mode = kMode;
	}
	if (depth) {
		*depth = 0;
	}
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
	TRACE("setDisplayMode(%d, %d)", (int)mode, (int)depth);
	return mode == kMode && depth == 0 ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
BeringiaFramebuffer::getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
	return getCurrentDisplayMode(mode, depth);
}

IOReturn
BeringiaFramebuffer::setStartupDisplayMode(IODisplayModeID mode, IOIndex depth)
{
	return mode == kMode && depth == 0 ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
BeringiaFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
	TRACE("getTimingInfoForDisplayMode(%d)", (int)mode);
	if (mode != kMode || !info) {
		return kIOReturnBadArgument;
	}
	bzero(info, sizeof(*info));
	info->appleTimingID = kIOTimingIDInvalid;
	info->flags = kIODetailedTimingValid;
	IODetailedTimingInformationV2 *t = &info->detailedInfo.v2;
	t->pixelClock = t->minPixelClock = t->maxPixelClock = pixelClock();
	t->horizontalActive = fWidth;
	t->horizontalBlanking = kHBlank;
	t->horizontalSyncOffset = kHFront;
	t->horizontalSyncPulseWidth = kHSync;
	t->verticalActive = fHeight;
	t->verticalBlanking = fVBlank;
	t->verticalSyncOffset = kVFront;
	t->verticalSyncPulseWidth = kVSync;
	t->numLinks = 1;
	return kIOReturnSuccess;
}

IOItemCount
BeringiaFramebuffer::getConnectionCount(void)
{
	return 1;
}

IOReturn
BeringiaFramebuffer::getAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t *value)
{
	IOReturn r;

	switch (attribute) {
	case kConnectionEnable:
	case kConnectionCheckEnable:
		if (value) {
			*value = 1;     /* a display is always connected and on */
		}
		r = kIOReturnSuccess;
		break;
	case kConnectionFlags:
		if (value) {
			*value = 0;     /* an external display: no backlight, no built-in panel */
		}
		r = kIOReturnSuccess;
		break;
	case kConnectionSupportsHLDDCSense:
		r = kIOReturnSuccess;  /* hasDDCConnect/getDDCBlock: the EDID */
		break;
	case kConnectionSupportsAppleSense:
	case kConnectionSupportsLLDDCSense:
		r = kIOReturnUnsupported;
		break;
	default:
		r = IOFramebuffer::getAttributeForConnection(connect, attribute, value);
		break;
	}
	TRACE("getAttributeForConnection(%d, '%c%c%c%c') -> %#x", (int)connect, (char)(attribute >> 24),
	    (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute, r);
	return r;
}

IOReturn
BeringiaFramebuffer::setAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t value)
{
	IOReturn r = IOFramebuffer::setAttributeForConnection(connect, attribute, value);

	TRACE("setAttributeForConnection(%d, '%c%c%c%c', %#lx) -> %#x", (int)connect, (char)(attribute >> 24),
	    (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute, (unsigned long)value, r);
	return r;
}

bool
BeringiaFramebuffer::hasDDCConnect(IOIndex connect)
{
	TRACE("hasDDCConnect(%d)", (int)connect);
	return connect == 0;
}

IOReturn
BeringiaFramebuffer::getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType, IOOptionBits options,
    UInt8 *data, IOByteCount *length)
{
	TRACE("getDDCBlock(%d, %u)", (int)connect, blockNumber);
	if (connect != 0 || blockNumber != 1 || blockType != kIODDCBlockTypeEDID || !data || !length ||
	    *length < sizeof(fEDID)) {
		return kIOReturnBadArgument;
	}
	memcpy(data, fEDID, sizeof(fEDID));
	*length = sizeof(fEDID);
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data)
{
	return kIOReturnSuccess;        /* no hardware gamma: accepted and ignored */
}

IOReturn
BeringiaFramebuffer::setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries, IOOptionBits options)
{
	return kIOReturnSuccess;        /* direct pixels only */
}

IOReturn
BeringiaFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
	IOReturn r = IOFramebuffer::getAttribute(attribute, value);

	TRACE("getAttribute('%c%c%c%c') -> %#x", (char)(attribute >> 24), (char)(attribute >> 16),
	    (char)(attribute >> 8), (char)attribute, r);
	return r;
}

IOReturn
BeringiaFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
	IOReturn r = IOFramebuffer::setAttribute(attribute, value);

	TRACE("setAttribute('%c%c%c%c', %#lx) -> %#x", (char)(attribute >> 24), (char)(attribute >> 16),
	    (char)(attribute >> 8), (char)attribute, (unsigned long)value, r);
	return r;
}

IOReturn
BeringiaFramebuffer::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, OSObject *target,
    void *ref, void **interruptRef)
{
	TRACE("registerForInterruptType('%c%c%c%c')", (char)(interruptType >> 24), (char)(interruptType >> 16),
	    (char)(interruptType >> 8), (char)interruptType);
	if (interruptType != kIOFBConnectInterruptType || !proc || !interruptRef) {
		return kIOReturnUnsupported;    /* no vertical blank */
	}
	fConnectProc = proc;
	fConnectTarget = target;
	fConnectRef = ref;
	*interruptRef = &fConnectProc;
	/* IOFramebuffer has its work loop by now (open time; there is none yet in start) */
	if (!fConnectTimer && getWorkLoop()) {
		fConnectTimer = IOTimerEventSource::timerEventSource(this, &BeringiaFramebuffer::raiseConnectChange);
		if (fConnectTimer && getWorkLoop()->addEventSource(fConnectTimer) != kIOReturnSuccess) {
			OSSafeReleaseNULL(fConnectTimer);
		}
	}
	if (!fConnectTimer) {
		IOLog("BeringiaFramebuffer: no timer for the connect-change interrupt\n");
	}
	if (!fDisplayNotifier) {
		OSDictionary *match = serviceMatching("IODisplay");

		/* gIOMatchedNotification also reports displays that are already there */
		fDisplayNotifier = match ? addMatchingNotification(gIOMatchedNotification, match,
		    &BeringiaFramebuffer::displayAppeared, this, NULL, 0) : NULL;
		OSSafeReleaseNULL(match);
	}
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::unregisterInterrupt(void *interruptRef)
{
	if (interruptRef != &fConnectProc) {
		return IOFramebuffer::unregisterInterrupt(interruptRef);
	}
	fConnectProc = NULL;
	return kIOReturnSuccess;
}

IOReturn
BeringiaFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
	if (interruptRef != &fConnectProc) {
		return IOFramebuffer::setInterruptState(interruptRef, state);
	}
	return kIOReturnSuccess;        /* raised at most once anyway */
}

/* BeringiaDisplayHost's way in: a new, initialised instance under the virtual PCI device. */
IOService *
beringia_framebuffer_create(void)
{
	BeringiaFramebuffer *fb = new BeringiaFramebuffer;

	if (!fb || !fb->init(NULL)) {
		if (fb) {
			fb->release();
		}
		return NULL;
	}
	return fb;
}

/* The kmod_info the kext linker needs (OpenCore's refuses a kext without one); IOKit manages the instances. */
extern "C" {
kern_return_t
beringia_fb_start(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

kern_return_t
beringia_fb_stop(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(com.beringia.framebuffer, "1.0.0",
    beringia_fb_start, beringia_fb_stop)
}
