// SPDX-License-Identifier: APSL-2.0
/*
 * IOGraphicsAccelerator2.h - source-level reconstruction of the closed KDK classes
 *   IOGraphicsAccelerator2   (IOAcceleratorFamily2.kext, com.apple.iokit.IOAcceleratorFamily2)
 *   IOAccelDisplayMachine    (same kext)
 *   IOAccelDisplayPipe       (same kext)
 *   IOAccelDisplayPipeTransaction2 (same kext, minimally: the plane getters)
 *
 * Draft for Beringia: subclass these from a kext (BeringiaAccelerator, BeringiaDisplayMachine,
 * BeringiaDisplayPipe).  Derived read-only from the KDK 25A349 x86_64 binaries:
 *   $BERINGIA_KDK_EXTENSIONS/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2
 *   $BERINGIA_KDK_EXTENSIONS/IOGraphicsFamily.kext/IOGraphicsFamily        (IOAccelerator)
 *   $BERINGIA_KDK_EXTENSIONS/AppleParavirtGPU.kext/Contents/MacOS/AppleParavirtGPU
 *     (Apple's own subclass of all three; used only to recover names/signatures of the
 *      pure-virtual slots, which have no symbol of their own in the base vtable)
 *
 * CLASS FACTS (from the binary, not guessed)
 * --------------------------------------------------------------------------------------
 *   IOGraphicsAccelerator2 : IOAccelerator : IOService
 *     - superClass slot in __const (__ZN22IOGraphicsAccelerator210superClassE,
 *       IOAF2 0x7b8d8) binds __ZN13IOAccelerator10gMetaClassE; IOAccelerator adds no
 *       virtual functions (SDK IOKit/graphics/IOAccelerator.h), so the first
 *       IOGraphicsAccelerator2 virtual is slot 266, immediately after IOService's 266.
 *     - __ZTV22IOGraphicsAccelerator2 = IOAF2 0x7b8e0; vptr = __ZTV+0x10.
 *     - 350 slots (idx 0..349): 0/1 destructor D1/D0, 2..265 IOService/OSObject/
 *       IORegistryEntry, 266..349 the 84 virtuals declared below (idx 350 is the
 *       null terminator before __ZTVN22IOGraphicsAccelerator29MetaClassE).
 *     - Pure (___cxa_pure_virtual) slots among the 84: 271, 273, 307, 310, 312, 313,
 *       314, 320, 324, 325, 326, 327, 328, 332, 333, 334, 337.  A concrete subclass
 *       must override those 17.  Names/signatures of those slots were read off
 *       AppleParavirtAccelerator's vtable (its overrides sit at the same slots).
 *     - IOGraphicsAccelerator2::start(IOService*) (0x3a91e) drives the display setup:
 *         *0x9c0 = configureDevice(provider)   at 0x3abf4 (must return true)
 *         *0x9b0 = populateAccelConfig(&this->config @0xc88) at 0x3aeb0
 *         *0x880 = createEventMachine()        at 0x3b264
 *         *0xa00 = newDisplayMachine()         at 0x3b325; stored at accel+0x378
 *         *0x850 on that machine = init(accel) at 0x3b36d
 *       It calls acceleratorDidLock/WillUnlock (0x850/0x858) around the work; the
 *       machine's start() (0x858 on the machine vtable) is NOT called from here.
 *
 *   IOAccelDisplayMachine : IOService
 *     - superClass binds __ZN9IOService10gMetaClassE (IOAF2 0x6fd98); first own virtual
 *       slot 266.  __ZTV21IOAccelDisplayMachine = 0x6fda0.  18 own virtuals (266..283).
 *     - start(IOPCIDevice*) at 0xdc9e is the enumerator: provider->getChildIterator(
 *       gIODTPlane), clients with category "IOFramebuffer", safeMetaCast to IOFramebuffer,
 *       then virtual *0x8d8 = found_framebuffer(fb); then provider->getClientIterator()
 *       and recursively each client's getClientIterator(), same cast and *0x8d8.
 *       found_framebuffer (0xe046) calls accelerator *0x908 = createDisplayPipe(fb,index).
 *     - Pure slots: 269, 270 (displayModeWillChange()/displayModeDidChange() in Apple's
 *       subclass) plus 271/272 which the base implements.  A concrete subclass must
 *       override 269 and 270.
 *     - init(IOGraphicsAccelerator2*) at 0xdc0e only stores the accelerator at +0x128 and
 *       allocates a lock; it returns void.
 *
 *   IOAccelDisplayPipe : IOService
 *     - superClass binds IOService gMetaClass; first own virtual slot 266.
 *       __ZTV18IOAccelDisplayPipe = 0x7d5d8.  40 own virtuals (266..305), none pure;
 *       the base class is concrete and supplies transaction queue, user-client plumbing
 *       and notifications.
 *     - init(accel, machine, fb, index) at 0x49528 stores accel@0x88 machine@0x90 fb@0x98
 *       index@0x284, creates the scanout resource via accelerator *0x8d0 = createResource(0,0x40),
 *       registers addFramebufferNotificationWithOptions(handler,this,index,0x170,0), copies
 *       IOCFPlugInTypes accel->fb, and sets fb properties IOAccelTypes (accel IOService path),
 *       IOAccelIndex (index) and IOAccelRevision (2).  Those three properties are what
 *       CoreDisplay uses to pair an IOFB service with its pipe.
 *     - The transaction path: user client type 4 (IOAccelDisplayPipeUserClient2, gated on
 *       accelerator config byte +0x47) -> IOAccelDisplayPipe::transaction_queue_gated
 *       (0x4d23f) -> *0x8b8 submitTransaction(txn).  Override submitTransaction to get
 *       the plane IOSurface from the transaction.
 *
 * PROPERTIES / GATES for the display-pipe path
 * --------------------------------------------------------------------------------------
 *   Accelerator personality "AcceleratorProperties":
 *     IOAccelDisplayPipeCapabilities = { DisplayPipeSupported = true;
 *                                        TransactionsSupported = true; }
 *     IOMatchCategory = IOAccelerator;     (personality IOMatchCategory, not a class prop)
 *   The pipe's copyCapabilities() (slot 0x8d0) reads the property
 *   "IOAccelDisplayPipeCapabilities" from the accelerator (getProperty slot 0x2e8).
 *   IOGraphicsAccelerator2::newUserClient type 4 tests the single byte at accelerator
 *   +0xccf; start() calls populateAccelConfig with rsi = accelerator+0xc88, so the
 *   subclass MUST write 1 at IOAccelConfig offset 0x47 (0xc88+0x47 = 0xccf) or
 *   WindowServer cannot open the display-pipe user client at all.  Apple's paravirt
 *   also writes u32 0x480000 at +0x08, u32 2 at +0x64 and u64 7 at +0x0c.
 *
 * Sizes/ivars used by the base (for reference; the subclass does not need them):
 *   IOGraphicsAccelerator2 config block 0xc88; machine ptr 0x378; event machine 0x380.
 */
#ifndef _BERINGIA_IO_GRAPHICS_ACCELERATOR2_H
#define _BERINGIA_IO_GRAPHICS_ACCELERATOR2_H

#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>

/* ---- opaque framework types referenced by the vtable signatures ----------------
 * Only pointers appear in the interfaces below, so forward declarations are enough.
 * The parameter type names are load-bearing: they must reproduce the kernel's mangled
 * vtable symbols exactly (e.g. createVidMemory takes IOAccelShared2*, IOAccelResource2*,
 * unsigned long long, void* -> ...EP14IOAccelShared2P16IOAccelResource2yPv). */
struct task;
class IOSurface;
class IOAccelShared2;
class IOAccelResource2;
class IOAccelTask;
class IOAccelMemory;
class IOAccelVidMemory;
class IOAccelSysMemory;
class IOAccelMemoryMap;
class IOAccelRemoteMemory;
class IOAccelCommandDescriptor;
class IOAccelBlockFence;
class IOAccelEventFence;
class IOAccelEvent;
class IOAccelStatistics;
class IOAccelDrawable;
class IOAccelSurface;
class IOAccelContext2;
class IOAccel2DContext2;
class IOAccelVideoContext2;
class IOAccelSharedUserClient2;
class IOAccelDevice2;
class IOAccelCommandQueue;
class IOAccelEventMachineFast2;
class IOAccelClientSharedRO;
class IOAccelClientSharedRW;
class IOAccelAllocationInfo;
class IOAccelConfig;
class IOAccelDisplayMachine;
class IOAccelDisplayPipe;
class IOAccelDisplayPipeScaler;
class IOAccelDisplayPipeTransaction2;
class IOGraphicsAccelerator2;
class IOBufferMemoryDescriptor;
class IOMemoryDescriptor;
class IODMACommand;
class IOWorkLoop;

/* ---- property keys and layout constants ---------------------------------------- */
#define kIOAccelDisplayPipeCapabilitiesKey  "IOAccelDisplayPipeCapabilities"
#define kDisplayPipeSupportedKey            "DisplayPipeSupported"
#define kTransactionsSupportedKey           "TransactionsSupported"

enum {
	/* IOAccelConfig byte that enables IOGraphicsAccelerator2 newUserClient(type 4) */
	kIOAccelConfigDisplayPipePresent    = 0x47,
	kIOAccelConfigValue08               = 0x08,   /* Apple: u32 0x00480000 */
	kIOAccelConfigValue0c               = 0x0c,   /* Apple: u64 7          */
	kIOAccelConfigValue64               = 0x64,   /* Apple: u32 2          */
	/* accelerator ivar offsets that the base reads (documentation only) */
	kIOGraphicsAccelerator2ConfigBase   = 0xc88,
	kIOGraphicsAccelerator2DisplayMachine = 0x378,
};

/* ==================================================================================
 * IOAccelDisplayPipeTransaction2 : OSObject
 *   superClass binds __ZN8OSObject10gMetaClassE (IOAF2 0x7e070).
 *   The plane accessors are ordinary (non-virtual) const methods exported by IOAF2:
 *     __ZNK30IOAccelDisplayPipeTransaction217getPlaneIOSurfaceEjj
 *     __ZNK30IOAccelDisplayPipeTransaction216getPlaneResourceEjj
 *   The class also has virtuals (newDisplayPipeTransaction, complete, finish, ...), but a
 *   pipe only needs the accessors, and declaring them non-virtual keeps the call an
 *   ordinary direct call to the exported symbol.
 * ================================================================================== */
class IOAccelDisplayPipeTransaction2 : public OSObject
{
public:
	/* plane 0 is the scanout plane; index is the per-pipe plane index. */
	IOSurface *getPlaneIOSurface(unsigned int plane, unsigned int index) const;
	IOAccelResource2 *getPlaneResource(unsigned int plane, unsigned int index) const;
};

/* ==================================================================================
 * IOAccelDisplayMachine : IOService
 *   vptr = __ZTV+0x10; own virtuals are slots 0x850..0x8d8 (idx 266..283).
 * ================================================================================== */
class IOAccelDisplayMachine : public IOService
{
	OSDeclareDefaultStructors(IOAccelDisplayMachine)
public:
	/* IOAccelDisplayMachine installs its own free() in the inherited slot 0x090
	 * (everything else below 0x850 is plain IOService). */
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	/* 0x850 */ virtual void init(IOGraphicsAccelerator2 *accelerator);
	/* 0x858 */ virtual bool start(IOPCIDevice *provider);
	/* 0x860 */ virtual void stop(void);
	/* 0x868 */ virtual void displayModeWillChange(void) = 0;      /* pure in base */
	/* 0x870 */ virtual void displayModeDidChange(void) = 0;       /* pure in base */
	/* 0x878 */ virtual void initFramebufferResource(unsigned int index, unsigned int count,
	                                                 IOFramebuffer *fb, IOAccelResource2 *resource);
	/* 0x880 */ virtual void destroyFramebufferResource(unsigned int index, unsigned int count,
	                                                    IOFramebuffer *fb, IOAccelResource2 *resource);
	/* 0x888 */ virtual void triage(char **strings, unsigned long long *count);
	/* 0x890..0x8b0 RESERVED */
	virtual void _RESERVEDIOAccelDisplayMachine1(void);
	virtual void _RESERVEDIOAccelDisplayMachine2(void);
	virtual void _RESERVEDIOAccelDisplayMachine3(void);
	virtual void _RESERVEDIOAccelDisplayMachine4(void);
	virtual void _RESERVEDIOAccelDisplayMachine5(void);
	/* 0x8b8 */ virtual void framebuffer_will_power_off(unsigned int index);
	/* 0x8c0 */ virtual void framebuffer_did_power_on(unsigned int index);
	/* 0x8c8 */ virtual void display_mode_will_change(unsigned int index);
	/* 0x8d0 */ virtual void display_mode_did_change(unsigned int index);
	/* 0x8d8 */ virtual void found_framebuffer(IOFramebuffer *fb);
};

/* ==================================================================================
 * IOAccelDisplayPipe : IOService
 *   vptr = __ZTV+0x10; own virtuals are slots 0x850..0x988 (idx 266..305), all concrete.
 * ================================================================================== */
class IOAccelDisplayPipe : public IOService
{
	OSDeclareDefaultStructors(IOAccelDisplayPipe)
public:
	/* IOAccelDisplayPipe installs its own free() in the inherited slot 0x090. */
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	/* 0x850 */ virtual bool init(IOGraphicsAccelerator2 *accelerator,
	                              IOAccelDisplayMachine *machine,
	                              IOFramebuffer *framebuffer, unsigned int index);
	/* 0x858 */ virtual void initFramebufferResource(unsigned int index, IOAccelResource2 *resource);
	/* 0x860 */ virtual void destroyFramebufferResource(unsigned int index, IOAccelResource2 *resource);
	/* 0x868 */ virtual void displayModeWillChange(void);
	/* 0x870 */ virtual void displayModeDidChange(void);
	/* 0x878 */ virtual void enableVBLInterrupt(void);
	/* 0x880 */ virtual void disableVBLInterrupt(void);
	/* 0x888 */ virtual void enableTransactionInterrupt(void);
	/* 0x890 */ virtual void disableTransactionInterrupt(void);
	/* 0x898 */ virtual IOAccelDisplayPipeTransaction2 *newDisplayPipeTransaction(void);
	/* 0x8a0 */ virtual bool validateTransaction(IOAccelDisplayPipeTransaction2 *txn);
	/* 0x8a8 */ virtual void performTransaction(IOAccelDisplayPipeTransaction2 *txn);
	/* 0x8b0 */ virtual bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn);
	/* 0x8b8 */ virtual void submitTransaction(IOAccelDisplayPipeTransaction2 *txn);
	/* 0x8c0 */ virtual void getDisplayModePipeScalerSetup(IOAccelDisplayPipeScaler *scaler);
	/* 0x8c8 */ virtual IOWorkLoop *createWorkLoop(void);
	/* 0x8d0 */ virtual OSObject *copyCapabilities(void);
	/* 0x8d8 */ virtual void beginTransaction(IOAccelEvent *event);
	/* 0x8e0 */ virtual void signalTransactionComplete(IOAccelEvent *event);
	/* 0x8e8 */ virtual void framebufferTerminated(void);
	/* 0x8f0..0x910 WSAA (WindowServer async access) hooks */
	virtual void wsaaEnterDefer(int reason);
	virtual void wsaaWillExitDefer(int reason);
	virtual void wsaaDidExitDefer(int reason);
	virtual void wsaaWillEnterDefer(int reason);
	virtual void wsaaDidEnterDefer(int reason);
	/* 0x918 */ virtual void willPowerOff(void);
	/* 0x920 */ virtual void didPowerOn(void);
	/* 0x928 */ virtual void logTransactionTimeoutDiagnosisReport(void);
	/* 0x930 */ virtual unsigned int getStampIndex(void) const;
	/* 0x938 */ virtual void triage(char **strings, unsigned long long *count);
	/* 0x940..0x970 RESERVED */
	virtual void _RESERVEDIOAccelDisplayPipe1(void);
	virtual void _RESERVEDIOAccelDisplayPipe2(void);
	virtual void _RESERVEDIOAccelDisplayPipe3(void);
	virtual void _RESERVEDIOAccelDisplayPipe4(void);
	virtual void _RESERVEDIOAccelDisplayPipe5(void);
	virtual void _RESERVEDIOAccelDisplayPipe6(void);
	virtual void _RESERVEDIOAccelDisplayPipe7(void);
	/* 0x978 */ virtual void framebuffer_will_power_off(void);
	/* 0x980 */ virtual void framebuffer_did_power_on(void);
	/* 0x988 */ virtual void framebuffer_terminated(void);
};

/* ==================================================================================
 * IOGraphicsAccelerator2 : IOAccelerator
 *   vptr = __ZTV+0x10; own virtuals are slots 0x850..0xae8 (idx 266..349), 84 in all.
 *   Slots marked "= 0" are pure in the KDK (___cxa_pure_virtual).
 * ================================================================================== */
class IOGraphicsAccelerator2 : public IOAccelerator
{
	OSDeclareDefaultStructors(IOGraphicsAccelerator2)
public:
	/* Overrides of inherited IOService/OSObject/IORegistryEntry slots that
	 * IOGraphicsAccelerator2 installs itself (KDK __ZTV22IOGraphicsAccelerator2
	 * slots 18, 96, 127, 129, 133, 134, 184, 185, 192, 193, 209, 229, 230, 239).
	 * They must be declared or a subclass vtable would point at plain IOService. */
	/* 0x090 */ virtual void free(void) APPLE_KEXT_OVERRIDE;
	/* 0x300 */ virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	/* 0x3f8 */ virtual bool requestTerminate(IOService *provider, IOOptionBits options) APPLE_KEXT_OVERRIDE;
	/* 0x408 */ virtual bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) APPLE_KEXT_OVERRIDE;
	/* 0x428 */ virtual IOReturn configureReport(IOReportChannelList *channels,
	                IOReportConfigureAction action, void *result, void *destination) APPLE_KEXT_OVERRIDE;
	/* 0x430 */ virtual IOReturn updateReport(IOReportChannelList *channels,
	                IOReportUpdateAction action, void *result, void *destination) APPLE_KEXT_OVERRIDE;
	/* 0x5c0 */ virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	/* 0x5c8 */ virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	/* 0x600 */ virtual bool terminate(IOOptionBits options) APPLE_KEXT_OVERRIDE;
	/* 0x608 */ virtual bool finalize(IOOptionBits options) APPLE_KEXT_OVERRIDE;
	/* 0x688 */ virtual IOWorkLoop *getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	/* 0x728 */ virtual IOReturn requestProbe(IOOptionBits options) APPLE_KEXT_OVERRIDE;
	/* 0x730 */ virtual IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE;
	/* 0x778 */ virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	                UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	/* ---- own virtuals ---- */
	/* 0x850 */ virtual void acceleratorDidLock(const char *name, int line);
	/* 0x858 */ virtual void acceleratorWillUnlock(const char *name, int line);
	/* 0x860 */ virtual unsigned long long tmpTotalVRAM(void);
	/* 0x868 */ virtual unsigned long long totalTextureMemory(void);
	/* 0x870 */ virtual bool isIdle(void);
	/* 0x878 */ virtual void *getStampMemory(unsigned int *index) = 0;
	/* 0x880 */ virtual IOAccelEventMachineFast2 *createEventMachine(void);
	/* 0x888 */ virtual IOAccelEventMachineFast2 *newEventMachine(void) = 0;
	/* 0x890 */ virtual IOAccelClientSharedRO *newClientSharedRO(void);
	/* 0x898 */ virtual IOAccelClientSharedRW *newClientSharedRW(void);
	/* 0x8a0 */ virtual void deleteClientSharedRO(IOAccelClientSharedRO *shared);
	/* 0x8a8 */ virtual void deleteClientSharedRW(IOAccelClientSharedRW *shared);
	/* 0x8b0 */ virtual IOAccelShared2 *createShared(task *task);
	/* 0x8b8 */ virtual IOAccelSysMemory *createSysMemory(void);
	/* 0x8c0 */ virtual IOAccelVidMemory *createVidMemory(IOAccelShared2 *shared,
	                                                    IOAccelResource2 *resource,
	                                                    unsigned long long offset, void *ptr);
	/* 0x8c8 */ virtual IOAccelMemoryMap *createMemoryMap(IOAccelTask *gpuTask,
	                                                    IOAccelMemory *memory, unsigned int flags);
	/* 0x8d0 */ virtual IOAccelResource2 *createResource(IOAccelShared2 *shared, unsigned int type);
	/* 0x8d8 */ virtual IOAccelCommandDescriptor *createCommandDescriptor(void);
	/* 0x8e0 */ virtual void deleteCommandDescriptor(IOAccelCommandDescriptor *desc);
	/* 0x8e8 */ virtual IOAccelBlockFence *newBlockFence(void);
	/* 0x8f0 */ virtual void scrubEvents(void);
	/* 0x8f8 */ virtual IOAccelDrawable *createDrawable(IOAccelSurface *surface,
	                                                   IOAccelShared2 *shared, bool flag);
	/* 0x900 */ virtual unsigned int resourceTypeForIOSurface(IOSurface *surface);
	/* 0x908 */ virtual IOAccelDisplayPipe *createDisplayPipe(IOFramebuffer *fb,
	                                                         unsigned int index);
	/* 0x910 */ virtual IOMemoryDescriptor *createMemoryDescriptorWithAddressRange(
	                unsigned long long address, unsigned long long length,
	                unsigned int options, task *task);
	/* 0x918 */ virtual IOMemoryDescriptor *createMemoryDescriptorWithPersistentMemoryDescriptor(
	                IOMemoryDescriptor *md);
	/* 0x920 */ virtual IOBufferMemoryDescriptor *createBufferMemoryDescriptorWithOptions(
	                unsigned int options, unsigned long capacity, unsigned long alignment);
	/* 0x928 */ virtual IOBufferMemoryDescriptor *createBufferMemoryDescriptorInTaskWithOptions(
	                task *task, unsigned int options, unsigned long capacity, unsigned long alignment);
	/* 0x930 */ virtual void freeAllGPUMappings(void);
	/* 0x938 */ virtual void freeWaitToPrepareVidMemory(IOAccelVidMemory *memory, bool wait, bool force);
	/* 0x940 */ virtual void freeWaitToPrepareVidMap(IOAccelMemoryMap *map, bool wait, bool force);
	/* 0x948 */ virtual void unwireAllVidMemory(void);
	/* 0x950 */ virtual void freeAllVidMemoryMappings(void);
	/* 0x958 */ virtual void pageoffSurfaceInLinear(void);
	/* 0x960 */ virtual void freeWaitToPrepareSysMemory(IOAccelSysMemory *memory, bool wait);
	/* 0x968 */ virtual void freeWaitToPrepareSysMap(IOAccelMemoryMap *map, bool wait);
	/* 0x970 */ virtual void collectGartWirings(void);
	/* 0x978 */ virtual void unwireAllSysMemory(void);
	/* 0x980 */ virtual void freeAllSysMemoryMappings(void);
	/* 0x988 */ virtual unsigned long long getMaxSingleSysMemorySize(void);
	/* 0x990 */ virtual unsigned long long getMaxWiredSysMemorySize(void);
	/* 0x998 */ virtual IOAccelTask *createUserGPUTask(void) = 0;
	/* 0x9a0 */ virtual void acceleratorFinalize(void);
	/* 0x9a8 */ virtual void submitSwapCopy(IOAccelEvent *event, IOAccelResource2 *src,
	                                       IOAccelResource2 *dst);
	/* 0x9b0 */ virtual void populateAccelConfig(IOAccelConfig *config) = 0;
	/* 0x9b8 */ virtual unsigned long long calcMaxGPUPhysicalMemoryBytes(unsigned long long bytes);
	/* 0x9c0 */ virtual bool configureDevice(IOPCIDevice *provider) = 0;
	/* 0x9c8 */ virtual void teardownDevice(IOPCIDevice *provider) = 0;
	/* 0x9d0 */ virtual IOAccelTask *createKernelGPUTask(void) = 0;
	/* 0x9d8 */ virtual void systemWillSleep(void);
	/* 0x9e0 */ virtual void systemDidWake(void);
	/* 0x9e8 */ virtual void systemWillChangeSpeed(void);
	/* 0x9f0 */ virtual void systemDidChangeSpeed(void);
	/* 0x9f8 */ virtual bool configIsHeadless(void);
	/* 0xa00 */ virtual IOAccelDisplayMachine *newDisplayMachine(void) = 0;
	/* 0xa08 */ virtual IOAccelShared2 *newShared(void);
	/* 0xa10 */ virtual IOAccelSharedUserClient2 *newSharedUserClient(void);
	/* 0xa18 */ virtual IOAccelDevice2 *newDevice(void);
	/* 0xa20 */ virtual IOAccelContext2 *newGLContext(void) = 0;
	/* 0xa28 */ virtual IOAccelContext2 *newCLContext(void) = 0;
	/* 0xa30 */ virtual IOAccelSurface *newSurface(void) = 0;
	/* 0xa38 */ virtual IOAccel2DContext2 *new2DContext(void) = 0;
	/* 0xa40 */ virtual IOAccelVideoContext2 *newVideoContext(void) = 0;
	/* 0xa48 */ virtual IOAccelDisplayPipe *newDisplayPipe(void);
	/* 0xa50 */ virtual IOAccelDrawable *newDrawable(void);
	/* 0xa58 */ virtual IOAccelContext2 *newContext(unsigned int type);
	/* 0xa60 */ virtual IOAccelSysMemory *newSysMemory(void) = 0;
	/* 0xa68 */ virtual IOAccelVidMemory *newVidMemory(void) = 0;
	/* 0xa70 */ virtual IOAccelResource2 *newResource(void) = 0;
	/* 0xa78 */ virtual IOAccelCommandDescriptor *newCommandDescriptor(void);
	/* 0xa80 */ virtual IOAccelStatistics *newStatistics(void);
	/* 0xa88 */ virtual IOAccelMemoryMap *newMemoryMap(void) = 0;
	/* 0xa90 */ virtual unsigned long long getMaxResourceSize(void);
	/* 0xa98 */ virtual IOAccelRemoteMemory *newRemoteMemory(void);
	/* 0xaa0 */ virtual IOAccelRemoteMemory *createRemoteMemory(IOAccelShared2 *shared,
	                                                           unsigned long long length,
	                                                           unsigned int a, unsigned int b);
	/* 0xaa8 */ virtual IOAccelEventFence *newContextEventFence(void);
	/* 0xab0 */ virtual IODMACommand *createIODMACommand(void);
	/* 0xab8 */ virtual void triage(char **strings, unsigned long long *count);
	/* 0xac0..0xad0 RESERVED */
	virtual void _RESERVEDIOGraphicsAccelerator3(void);
	virtual void _RESERVEDIOGraphicsAccelerator4(void);
	virtual void _RESERVEDIOGraphicsAccelerator5(void);
	/* 0xad8 */ virtual void describeDriverAllocations(IOAccelAllocationInfo *info);
	/* 0xae0 */ virtual IOAccelCommandQueue *newCommandQueue(void);
	/* 0xae8 */ virtual void ktraceStateChanged(void);
};

#endif /* _BERINGIA_IO_GRAPHICS_ACCELERATOR2_H */
