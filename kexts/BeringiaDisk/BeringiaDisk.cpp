// SPDX-License-Identifier: APSL-2.0
/*
 * BeringiaDisk: a synchronous host-file-backed block device for hosted x86_64 XNU.
 *
 * Design informed by xnulinux's XnuDisk (APSL-2.0). Apple IOStorageFamily's
 * IOBlockStorageDriver attaches to this device and publishes the whole IOMedia;
 * its partition-scheme personalities then publish any GPT partition media.
 *
 * The host ABI carries physical addresses, not arbitrary XNU virtual addresses.
 * One IOBufferMemoryDescriptor therefore owns a physically contiguous request
 * page and bounce buffer. This is deliberately simple: an IOLock serializes all
 * requests and every host call completes before the IOKit completion is called.
 */
#include <TargetConditionals.h>
#undef TARGET_OS_OSX
#define TARGET_OS_OSX 1

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
#include <beringia/abi.h>
}

namespace {
constexpr IOByteCount kRequestBytes = 4096;
constexpr IOByteCount kBounceBytes = 1024 * 1024;
static_assert(sizeof(beringia_block_req) <= kRequestBytes);

/* Same SysV host-call convention as osfmk/x86_64/beringia.c. Keeping the tiny
 * call stub here avoids importing the kernel's non-KPI beringia_call symbol. */
static long
hostCall(UInt64 hook, unsigned long nr, unsigned long a1, unsigned long a2)
{
	long result;

	__asm__ volatile("call *%[hook]"
	    : "=a"(result), "+D"(nr), "+S"(a1), "+d"(a2)
	    : [hook] "r"(hook)
	    : "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
	return result;
}
}

class BeringiaDisk : public IOBlockStorageDevice {
	OSDeclareDefaultStructors(BeringiaDisk)

	UInt64 fBlocks = 0;
	bool fReadOnly = false;
	UInt64 fHostCall = 0;
	IOLock *fLock = nullptr;
	IOBufferMemoryDescriptor *fDMA = nullptr;
	beringia_block_req *fRequest = nullptr;
	UInt8 *fBounce = nullptr;
	UInt64 fRequestPA = 0;
	UInt64 fBouncePA = 0;

	char fVendor[9] = "Beringia";
	char fProduct[24] = "Beringia Disk";
	char fRevision[8] = "1.0";
	char fInfo[4] = "";

public:
	bool start(IOService *provider) override;
	void free() override;

	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
	UInt32 doGetFormatCapacities(UInt64 *capacities, UInt32 maxCount) const override
	{
		if (capacities && maxCount)
			capacities[0] = fBlocks * BERINGIA_BLOCK_SIZE;
		return 1;
	}
	char *getVendorString() override { return fVendor; }
	char *getProductString() override { return fProduct; }
	char *getRevisionString() override { return fRevision; }
	char *getAdditionalDeviceInfoString() override { return fInfo; }
	IOReturn reportBlockSize(UInt64 *size) override
	{
		*size = BERINGIA_BLOCK_SIZE;
		return kIOReturnSuccess;
	}
	IOReturn reportEjectability(bool *ejectable) override
	{
		*ejectable = false;
		return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *block) override
	{
		*block = fBlocks - 1;
		return kIOReturnSuccess;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override
	{
		*present = true;
		if (changed)
			*changed = false;
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *removable) override
	{
		*removable = false;
		return kIOReturnSuccess;
	}
	IOReturn reportWriteProtection(bool *protect) override
	{
		*protect = fReadOnly;
		return kIOReturnSuccess;
	}
	IOReturn getWriteCacheState(bool *enabled) override
	{
		*enabled = false;
		return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override { return kIOReturnSuccess; }
	IOReturn doSynchronize(UInt64 block, UInt64 nblks,
	    IOStorageSynchronizeOptions options) override;
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
	    UInt64 nblks, IOStorageAttributes *attributes,
	    IOStorageCompletion *completion) override;

private:
	static void publishAfterBSD(void *self, wait_result_t result);
};

OSDefineMetaClassAndStructors(BeringiaDisk, IOBlockStorageDevice)

bool
BeringiaDisk::start(IOService *provider)
{
	beringia_block_info *info;
	IOByteCount segmentLength = 0;
	UInt64 bytes;

	if (!IOBlockStorageDevice::start(provider))
		return false;
	if (!PE_parse_boot_argn(BERINGIA_HOSTCALL_BOOT_ARG, &fHostCall,
	    sizeof(fHostCall)) || !fHostCall) {
		IOLog("BeringiaDisk: no %s boot argument; hosted loader is required\n",
		    BERINGIA_HOSTCALL_BOOT_ARG);
		return false;
	}

	fDMA = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    (IOOptionBits)kIOMemoryPhysicallyContiguous | (IOOptionBits)kIODirectionOutIn,
	    kRequestBytes + kBounceBytes, ~(mach_vm_address_t)0);
	if (!fDMA)
		return false;
	fRequest = static_cast<beringia_block_req *>(fDMA->getBytesNoCopy());
	fBounce = static_cast<UInt8 *>(fDMA->getBytesNoCopy()) + kRequestBytes;
	fRequestPA = fDMA->getPhysicalSegment(0, &segmentLength,
	    kIOMemoryMapperNone);
	if (!fRequest || !fRequestPA || segmentLength < kRequestBytes + kBounceBytes)
		return false;
	fBouncePA = fRequestPA + kRequestBytes;
	fLock = IOLockAlloc();
	if (!fLock)
		return false;

	memset(fRequest, 0, kRequestBytes);
	info = reinterpret_cast<beringia_block_info *>(fRequest);
	info->unit = 0;
	if (hostCall(fHostCall, BERINGIA_CALL_BLOCK_INFO, fRequestPA, 0) != 0 ||
	    info->block_size != BERINGIA_BLOCK_SIZE ||
	    info->size < BERINGIA_BLOCK_SIZE ||
	    (info->size % BERINGIA_BLOCK_SIZE) != 0) {
		IOLog("BeringiaDisk: no usable host disk (loader parameter disk=PATH)\n");
		return false;
	}
	fBlocks = info->size / BERINGIA_BLOCK_SIZE;
	bytes = info->size;
	fReadOnly = info->read_only != 0;

	/* Make the standard driver and filesystems see an internal virtual disk. */
	OSDictionary *protocol = OSDictionary::withCapacity(2);
	OSDictionary *device = OSDictionary::withCapacity(2);
	if (protocol && device) {
		OSString *virtualType = OSString::withCString(
		    kIOPropertyPhysicalInterconnectTypeVirtual);
		OSString *internal = OSString::withCString(kIOPropertyInternalKey);
		OSString *product = OSString::withCString(fProduct);
		OSString *vendor = OSString::withCString(fVendor);
		if (virtualType && internal && product && vendor) {
			protocol->setObject(kIOPropertyPhysicalInterconnectTypeKey,
			    virtualType);
			protocol->setObject(kIOPropertyPhysicalInterconnectLocationKey,
			    internal);
			device->setObject(kIOPropertyProductNameKey, product);
			device->setObject(kIOPropertyVendorNameKey, vendor);
			setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
			setProperty(kIOPropertyDeviceCharacteristicsKey, device);
		}
		OSSafeReleaseNULL(virtualType);
		OSSafeReleaseNULL(internal);
		OSSafeReleaseNULL(product);
		OSSafeReleaseNULL(vendor);
	}
	OSSafeReleaseNULL(protocol);
	OSSafeReleaseNULL(device);

	/* Publishing before IOBSD can deadlock APFS against IOResources matching.
	 * Wait on a separate thread, as a disk below a real controller would arrive
	 * after the BSD resource rather than synchronously inside that matching. */
	thread_t thread;
	retain();
	if (kernel_thread_start(&BeringiaDisk::publishAfterBSD, this,
	    &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		release();
		registerService();
	}
	IOLog("BeringiaDisk: %llu blocks of %u bytes (%llu MiB), %s\n",
	    (unsigned long long)fBlocks, BERINGIA_BLOCK_SIZE,
	    (unsigned long long)(bytes >> 20),
	    fReadOnly ? "read-only" : "writable");
	return true;
}

void
BeringiaDisk::publishAfterBSD(void *arg, wait_result_t)
{
	BeringiaDisk *self = static_cast<BeringiaDisk *>(arg);
	OSDictionary *match = IOService::resourceMatching("IOBSD");
	IOService *bsd = match ? IOService::waitForMatchingService(match) : nullptr;

	OSSafeReleaseNULL(match);
	OSSafeReleaseNULL(bsd);
	self->registerService();
	self->release();
	thread_terminate(current_thread());
}

void
BeringiaDisk::free()
{
	OSSafeReleaseNULL(fDMA);
	if (fLock) {
		IOLockFree(fLock);
		fLock = nullptr;
	}
	IOBlockStorageDevice::free();
}

IOReturn
BeringiaDisk::doSynchronize(UInt64, UInt64, IOStorageSynchronizeOptions)
{
	long callResult;
	long long result;

	IOLockLock(fLock);
	memset(fRequest, 0, sizeof(*fRequest));
	fRequest->unit = 0;
	fRequest->write = BERINGIA_BLOCK_FLUSH;
	fRequest->result = -5; /* -EIO if the old host ignores the request */
	callResult = hostCall(fHostCall, BERINGIA_CALL_BLOCK_RW, fRequestPA, 0);
	result = fRequest->result;
	IOLockUnlock(fLock);
	return callResult == 0 && result == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn
BeringiaDisk::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
    UInt64 nblks, IOStorageAttributes *, IOStorageCompletion *completion)
{
	IOByteCount total;
	IOByteCount done = 0;
	IOReturn status;
	bool write;

	if (!buffer || block > fBlocks || nblks > fBlocks - block)
		return kIOReturnBadArgument;
	write = buffer->getDirection() == kIODirectionOut;
	if (!write && buffer->getDirection() != kIODirectionIn)
		return kIOReturnBadArgument;
	if (write && fReadOnly)
		return kIOReturnNotWritable;
	total = nblks * BERINGIA_BLOCK_SIZE;
	if (total == 0) {
		IOStorage::complete(completion, kIOReturnSuccess, 0);
		return kIOReturnSuccess;
	}

	status = buffer->prepare();
	if (status == kIOReturnSuccess) {
		IOLockLock(fLock);
		while (done < total) {
			IOByteCount piece = total - done;
			long callResult;

			if (piece > kBounceBytes)
				piece = kBounceBytes;
			if (write && buffer->readBytes(done, fBounce, piece) != piece) {
				status = kIOReturnIOError;
				break;
			}
			memset(fRequest, 0, sizeof(*fRequest));
			fRequest->unit = 0;
			fRequest->write = write ? BERINGIA_BLOCK_WRITE : BERINGIA_BLOCK_READ;
			fRequest->offset = block * BERINGIA_BLOCK_SIZE + done;
			fRequest->nseg = 1;
			fRequest->result = -5; /* -EIO */
			fRequest->seg[0].pa = fBouncePA;
			fRequest->seg[0].len = piece;
			callResult = hostCall(fHostCall, BERINGIA_CALL_BLOCK_RW,
			    fRequestPA, 0);
			if (callResult != 0 || fRequest->result != (long long)piece) {
				IOLog("BeringiaDisk: %s at block %llu, length %llu failed "
				    "(call %ld, result %lld)\n", write ? "write" : "read",
				    (unsigned long long)(block + done / BERINGIA_BLOCK_SIZE),
				    (unsigned long long)piece, callResult, fRequest->result);
				status = kIOReturnIOError;
				break;
			}
			if (!write && buffer->writeBytes(done, fBounce, piece) != piece) {
				status = kIOReturnIOError;
				break;
			}
			done += piece;
		}
		IOLockUnlock(fLock);
		buffer->complete();
	}
	IOStorage::complete(completion, status,
	    status == kIOReturnSuccess ? total : done);
	return kIOReturnSuccess;
}

extern "C" {
kern_return_t
beringia_disk_start(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

kern_return_t
beringia_disk_stop(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(com.beringia.disk, "1.0.0",
    beringia_disk_start, beringia_disk_stop)
}
