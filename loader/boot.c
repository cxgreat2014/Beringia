// SPDX-License-Identifier: APSL-2.0
/*
 * The boot environment a booter gives x86_64 XNU (docs/plan.md, M2): boot_args, an EFI memory map of the hosted RAM,
 * a flattened device tree, the shared Beringia block, and page tables for vstart.
 *
 * Layout in the low carve-out, right after the kernel image (all below physfree, which XNU keeps):
 *   boot_args (4 KiB) | EFI memory map (4 KiB) | device tree (16 KiB) | boot PML4, identity PDPT, kernel PDPT
 */
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include "loader.h"
#include "xnu_boot.h"

#define PTE_P   (1ULL << 0)
#define PTE_W   (1ULL << 1)
#define PTE_PS  (1ULL << 7)
#define GiB     (1ULL << 30)
#define RAMDISK_HEADROOM (32U << 20)	/* left free after a RAM disk for XNU's early allocations (load_ramdisk) */

/* ---- device tree ---- */

struct dt_builder {
	u8 *base, *p, *end;
};

static void *dt_put(struct dt_builder *b, u32 len)
{
	void *r = b->p;

	if (b->p + len > b->end)
		return NULL;
	memset(b->p, 0, len);
	b->p += len;
	return r;
}

static struct xnu_dt_node *dt_node(struct dt_builder *b, u32 nprops, u32 nchildren)
{
	struct xnu_dt_node *n = dt_put(b, sizeof(*n));

	if (n) {
		n->nProperties = nprops;
		n->nChildren = nchildren;
	}
	return n;
}

static void dt_prop(struct dt_builder *b, const char *name, const void *val, u32 len)
{
	struct xnu_dt_prop *p = dt_put(b, sizeof(*p));
	void *v;

	if (!p)
		return;
	strscpy(p->name, name, sizeof(p->name));
	p->length = len;
	v = dt_put(b, (len + 3) & ~3U);
	if (v && len)
		memcpy(v, val, len);
}

static void dt_str(struct dt_builder *b, const char *name, const char *s)
{
	dt_prop(b, name, s, strlen(s) + 1);
}

static void dt_u64(struct dt_builder *b, const char *name, u64 v)
{
	dt_prop(b, name, &v, sizeof(v));
}

/* debug: recursively log a flattened DT node's name and its children */
static u8 *dt_debug_walk(u8 *p, int depth)
{
	u32 np = *(u32 *)p, nc = *(u32 *)(p + 4), i, len;
	u8 *q = p + 8;
	char name[33] = {0};

	for (i = 0; i < np; i++) {
		len = *(u32 *)(q + 32);
		if (i == 0)	/* the first property is "name"; its value is the node's name */
			memcpy(name, q + 36, len < 32 ? len : 32);
		q += 36 + ((len + 3) & ~3U);
	}
	bh_logf("xnu: DT %*s '%s' (%u children)", depth * 2, "", name, nc);
	for (i = 0; i < nc; i++)
		q = dt_debug_walk(q, depth + 1);
	return q;
}

static void dt_u32(struct dt_builder *b, const char *name, u32 v)
{
	dt_prop(b, name, &v, sizeof(v));
}

/* /, /chosen, /chosen/memory-map, /efi, /efi/platform, /options[, /display] */
static u64 build_dt(u8 *base, u64 room)
{
	struct dt_builder b = { base, base, base + room };

	/* with a framebuffer: the cell sizes /display's "reg" is read with (IODTResolveAddressing) */
	dt_node(&b, ldr.fb_size ? 5 : 3, ldr.fb_size ? 4 : 3);
	dt_str(&b, "name", "device-tree");
	dt_str(&b, "compatible", "Beringia1,1");
	dt_str(&b, "model", "Beringia1,1");
	if (ldr.fb_size) {
		dt_u32(&b, "#address-cells", 2);
		dt_u32(&b, "#size-cells", 1);
	}
	{
		u8 seed[64];

		bh_random(seed, sizeof(seed));	/* what the booter hands the kernel for its early entropy */
		dt_node(&b, 2, 1);
		dt_str(&b, "name", "chosen");
		dt_prop(&b, "random-seed", seed, sizeof(seed));
		{
			u32 dtmap[2];

			dt_node(&b, 2, 0);
			dt_str(&b, "name", "memory-map");
			/* IODeviceTreeAlloc needs /chosen/memory-map/DeviceTree (addr, size) to copy the DT's property tables
			 * out of the boot data (otherwise they alias it and dangle once the memory is reused). */
			dtmap[0] = (u32)ldr.dt_pa;
			dtmap[1] = 16384;
			dt_prop(&b, "DeviceTree", dtmap, sizeof(dtmap));
		}
	}
	{
		/*
		 * The machine identity, as the EFI booter supplies it: IOPlatformExpertDevice::generatePlatformUUID() derives
		 * IOPlatformUUID (what gethostuuid() waits for) from /efi/platform/system-id on x86. The VMware-style
		 * /options/platform-uuid fallback does not work here: IODTNVRAM takes over /options and hides its DT
		 * properties. A fixed value for bring-up; it should become per installation.
		 */
		static const u8 system_id[16] = {
			0xb5, 0xc4, 0x7f, 0x0e, 0x2d, 0x51, 0x4b, 0x5e,
			0x9e, 0x7c, 0x5a, 0x7d, 0x3f, 0x90, 0xc8, 0x26,
		};

		dt_node(&b, 1, 1);
		dt_str(&b, "name", "efi");
		{
			dt_node(&b, ldr.have_osk ? 5 : 4, 0);
			dt_str(&b, "name", "platform");
			dt_u64(&b, "FSBFrequency", bh_tsc_hz());
			dt_u64(&b, "ARTFrequency", 0);
			dt_prop(&b, "system-id", system_id, sizeof(system_id));
			/* the user's DSMOS key (loader main.c, read_osk); BeringiaSMC takes it and removes the property */
			if (ldr.have_osk)
				dt_prop(&b, "beringia-osk", ldr.osk, sizeof(ldr.osk));
		}
	}
	{
		/* /options: IODTPlatformExpert::processTopLevel requires it (gIOOptionsEntry, the NVRAM node) */
		dt_node(&b, 2, 0);
		dt_str(&b, "name", "options");
		dt_str(&b, "boot-args", "");
	}
	if (ldr.fb_size) {
		/*
		 * The display the boot framebuffer belongs to. The platform expert makes an IOPlatformDevice of it, which
		 * IONDRVSupport's IONDRVFramebuffer matches by name ("display"); with no NDRV in the device it falls back to
		 * IOBootNDRV, which drives the boot framebuffer (boot_args Video) when one of the device's memory ranges
		 * ("reg": address hi, lo; size) contains it. "AAPL,boot-display" makes it the console device.
		 */
		u32 reg[3] = { (u32)(ldr.fb_pa >> 32), (u32)ldr.fb_pa, (u32)ldr.fb_size };

		dt_node(&b, 4, 0);
		dt_str(&b, "name", "display");
		dt_str(&b, "device_type", "display");
		dt_prop(&b, "reg", reg, sizeof(reg));
		dt_prop(&b, "AAPL,boot-display", NULL, 0);
	}
	return b.p - b.base;
}

/*
 * The boot framebuffer (loader fb=WxH): 32 bpp, at the top of the low carve-out, taken out of the RAM XNU is given
 * (it is not in the EFI memory map, like a real one), so XNU maps it as device memory. Linux reads the same memory
 * through /dev/beringia-fb (bh_fb_publish, after the boot environment is built).
 */
static int reserve_fb(void)
{
	u64 size, top = ldr.low.pa + ldr.low.size, j;
	u32 *px;
	int i;

	if (!ldr.fb_width)
		return 0;
	ldr.fb_stride = ldr.fb_width * 4;
	size = ((u64)ldr.fb_stride * ldr.fb_height + (2ULL << 20) - 1) & ~((2ULL << 20) - 1);
	if (size > ldr.low.size / 8 || top - size < ldr.physfree) {
		bh_logf("xnu: no room for a %ux%u framebuffer in the low carve-out", ldr.fb_width, ldr.fb_height);
		return -ENOSPC;
	}
	px = bh_phys(top - size, size);
	if (!px)
		return -EFAULT;
	for (i = 0; i < ldr.nranges; i++)
		if (ldr.ranges[i].pa == ldr.low.pa)
			ldr.ranges[i].size -= size;
	ldr.low.size -= size;
	ldr.fb_pa = top - size;
	ldr.fb_size = size;
	/* a dark grey, not black: a screenshot of it shows the device works before XNU draws anything */
	for (j = 0; j < size / 4; j++)
		px[j] = 0x00303030;
	bh_logf("xnu: framebuffer %ux%u at PA %#llx (%llu KiB)", ldr.fb_width, ldr.fb_height, ldr.fb_pa, size >> 10);
	return 0;
}

/* ---- page tables for vstart ---- */

static int build_page_tables(u64 pa)
{
	u64 *pml4 = bh_phys(pa, 3 * 4096), *id = pml4 + 512, *k = pml4 + 1024;
	static bh_u64 linux_pdpt[512];
	int i;

	if (!pml4)
		return -ENOMEM;
	memset(pml4, 0, 3 * 4096);
	/* the low 4 GiB identity mapped (vstart's early code addresses physfree physically) and at KERNEL_BASE */
	pml4[0] = (pa + 4096) | PTE_P | PTE_W;
	pml4[511] = (pa + 8192) | PTE_P | PTE_W;
	for (i = 0; i < 4; i++) {
		id[i] = (i * GiB) | PTE_P | PTE_W | PTE_PS;
		k[i] = (i * GiB) | PTE_P | PTE_W | PTE_PS;
	}
	/* the host's kernel image, modules and fixmap: where the world switch and the host-call entry are */
	bh_linux_pdpt(linux_pdpt);
	k[510] = linux_pdpt[510];
	k[511] = linux_pdpt[511];
	return 0;
}

/* ---- the shared block ---- */

static int fill_shared(void)
{
	static bh_u64 linux_pdpt[512];
	unsigned char used[512];
	u64 *hook = ldr_kva(ldr.sym_hook, 8), top = 0;
	int i, dbl = 0, ceil_idx = 444;	/* Linux's EFI runtime region starts at PDPT 444 (0xffffffef00000000) */

	ldr.shared = ldr_kva(ldr.sym_shared, sizeof(*ldr.shared));
	if (!ldr.shared || !hook)
		return -EFAULT;
	memset(ldr.shared, 0, sizeof(*ldr.shared));
	bh_pml4_used(used);
	/* XNU's physmap goes to slot 509 (KERNEL_KEXTS_INDEX - 1), outside Linux's randomized range 273..507 */
	if (used[509]) {
		bh_log("xnu: PML4 slot 509 is in use by Linux");
		return -EBUSY;
	}
	for (i = 507; i >= 256 && !dbl; i--)
		if (!used[i])
			dbl = i;
	bh_linux_pdpt(linux_pdpt);
	for (i = 4; i < 510; i++) {
		if (linux_pdpt[i] & PTE_P) {
			ceil_idx = min(ceil_idx, i);
			break;
		}
	}
	for (i = 0; i < ldr.nranges; i++)
		top = max(top, ldr.ranges[i].pa + ldr.ranges[i].size);
	ldr.shared->abi_version = BERINGIA_ABI_VERSION;
	ldr.shared->ncpus = ldr.ncpus;
	ldr.shared->tsc_hz = bh_tsc_hz();
	ldr.shared->linux_pdpt_510 = linux_pdpt[510];
	ldr.shared->linux_pdpt_511 = linux_pdpt[511];
	ldr.shared->physmap_pml4 = 509;
	ldr.shared->dblmap_pml4 = dbl;
	ldr.shared->kva_ceiling = 0xffffff8000000000ULL + ((u64)ceil_idx << 30);
	ldr.shared->phys_top = top;
	for (i = 0; i < BERINGIA_MAX_CPUS; i++)
		ldr.shared->cpu[i].apic_id = i;
	*hook = bh_hostcall_entry();
	bh_logf("xnu: physmap slot 509, double-map slot %d, kernel VA below %#llx, RAM top %#llx",
		dbl, ldr.shared->kva_ceiling, top);
	return 0;
}

/*
 * A memory disk for XNU's memdev (boot args pmd0=base.size, rd=md0): the image sits right after the boot data,
 * 2 MiB aligned, inside the used extent boot_args reports (kaddr + ksize), so XNU's early page tables go after it.
 */
static int load_ramdisk(void)
{
	u64 size, off, pa;
	void *f;
	u8 *dst;

	if (!ldr.ramdisk_path)
		return 0;
	f = bh_file_open(ldr.ramdisk_path, 0, &size);
	if (!f) {
		bh_logf("xnu: cannot open ramdisk %s", ldr.ramdisk_path);
		return -ENOENT;
	}
	if (!size || (size & 4095)) {
		bh_logf("xnu: ramdisk size %#llx is not a nonzero multiple of 4096", size);
		bh_file_close(f);
		return -EINVAL;
	}
	pa = (ldr.physfree + (2ULL << 20) - 1) & ~((2ULL << 20) - 1);
	/*
	 * Room after it: XNU's early allocator (i386_init's ALLOCPAGES, the early page tables) takes pages from
	 * kaddr + ksize, i.e. right after the RAM disk, without a bound check. Ending the RAM disk at the end of the
	 * carve-out would let those writes run into host memory.
	 */
	if (size > ldr.low.pa + ldr.low.size || pa > ldr.low.pa + ldr.low.size - size ||
	    ldr.low.pa + ldr.low.size - (pa + size) < RAMDISK_HEADROOM) {
		bh_logf("xnu: ramdisk of %#llx bytes does not fit below %#llx with %u MiB to spare", size,
			ldr.low.pa + ldr.low.size, RAMDISK_HEADROOM >> 20);
		bh_file_close(f);
		return -ENOSPC;
	}
	dst = bh_phys(pa, size);
	if (!dst) {
		bh_file_close(f);
		return -ENOSPC;
	}
	for (off = 0; off < size;) {
		long r = bh_file_rw(f, 0, dst + off, min_t(u64, size - off, 1 << 20), off);

		if (r <= 0) {
			bh_file_close(f);
			return r < 0 ? r : -EIO;
		}
		off += r;
	}
	bh_file_close(f);
	ldr.ramdisk_pa = pa;
	ldr.ramdisk_size = size;
	bh_logf("xnu: ramdisk %s at PA %#llx, %#llx bytes (memdev pmd0, root rd=md0)", ldr.ramdisk_path, pa, size);
	return 0;
}

int ldr_boot_build(const char *cmdline)
{
	struct xnu_boot_args *ba;
	struct xnu_efi_range *mm;
	u64 pa = (ldr.kend + 4095) & ~4095ULL, total = 0;
	int i, n = 0, err;

	ldr.boot_args_pa = pa;
	ldr.memmap_pa = pa + 4096;
	ldr.dt_pa = pa + 8192;
	ldr.pml4_pa = pa + 8192 + 16384;
	ldr.physfree = ldr.pml4_pa + 3 * 4096;
	ba = bh_phys(ldr.boot_args_pa, 4096);
	mm = bh_phys(ldr.memmap_pa, 4096);
	if (!ba || !mm || !bh_phys(ldr.dt_pa, 16384) || ldr.physfree > ldr.low.pa + ldr.low.size)
		return -ENOSPC;
	err = reserve_fb();	/* first: it shrinks the low range the RAM disk must fit in */
	if (err)
		return err;
	err = load_ramdisk();
	if (err)
		return err;

	memset(ba, 0, sizeof(*ba));
	memset(mm, 0, 4096);
	for (i = 0; i < ldr.nranges; i++) {
		mm[n].Type = kEfiConventionalMemory;
		mm[n].PhysicalStart = ldr.ranges[i].pa;
		mm[n].NumberOfPages = ldr.ranges[i].size >> 12;
		total += ldr.ranges[i].size;
		n++;
	}
	ldr.dt_len = build_dt(bh_phys(ldr.dt_pa, 16384), 16384);

	ba->Revision = kBootArgsRevision1;
	ba->Version = kBootArgsVersion2;
	ba->efiMode = kBootArgsEfiMode64;
	/*
	 * The command line: [pmd0=RAMDISK ]user args beringia_hc=ENTRY[ rd=md0]. pmd0 goes first because XNU's memdev
	 * scans only the first 256 bytes of it by hand; rd=md0 goes last so a root the user names wins. beringia_hc:
	 * BeringiaDisk uses the exported PE_parse_boot_argn KPI instead of importing the kernel's non-KPI beringia_call;
	 * it is exactly the entry address fill_shared writes to beringia_hook, in the host-module mapping shared with XNU.
	 * Truncation is an error, never a silently shortened number.
	 */
	{
		size_t room = sizeof(ba->CommandLine), used = 0;
		int added = 0;

		if (ldr.ramdisk_size)
			added = snprintf(ba->CommandLine, room, "pmd0=%#llx.%#llx ", ldr.ramdisk_pa, ldr.ramdisk_size);
		if (added < 0 || (size_t)added >= room)
			return -E2BIG;
		used = added;
		added = snprintf(ba->CommandLine + used, room - used, "%s %s=%#llx%s", cmdline,
		    BERINGIA_HOSTCALL_BOOT_ARG, (unsigned long long)bh_hostcall_entry(), ldr.ramdisk_size ? " rd=md0" : "");
		if (added < 0 || (size_t)added >= room - used) {
			bh_logf("xnu: boot-args too long (%zu bytes available)", room);
			return -E2BIG;
		}
	}
	ba->MemoryMap = ldr.memmap_pa;
	ba->MemoryMapSize = n * sizeof(*mm);
	ba->MemoryMapDescriptorSize = sizeof(*mm);
	ba->MemoryMapDescriptorVersion = 1;
	ba->deviceTreeP = ldr.dt_pa;
	ba->deviceTreeLength = ldr.dt_len;
	if (ldr.fb_size) {
		ba->Video.v_display = 1;	/* GRAPHICS_MODE */
		ba->Video.v_rowBytes = ldr.fb_stride;
		ba->Video.v_width = ldr.fb_width;
		ba->Video.v_height = ldr.fb_height;
		ba->Video.v_depth = 32;
		ba->Video.v_baseAddr = ldr.fb_pa;
	}
	/* debug: walk the DT and log every node's name, to verify /options is present */
	{
		u8 *base = bh_phys(ldr.dt_pa, ldr.dt_len);

		bh_logf("xnu: DT debug: dt_pa=%#llx dt_len=%llu base=%p", ldr.dt_pa, ldr.dt_len, base);
		if (base)
			dt_debug_walk(base, 0);
	}
	ba->kaddr = ldr.kaddr;
	/* ksize covers the kernel, the boot data AND the RAM disk: XNU places its early page tables (KPTphys, from
	 * Idle_PTs_init) at kaddr + ksize, so the RAM disk must be inside the used extent or they overwrite it. */
	ba->ksize = (ldr.ramdisk_size ? ldr.ramdisk_pa + ldr.ramdisk_size : ldr.physfree) - ldr.kaddr;
	ba->kslide = ldr.slide;
	ba->PhysicalMemorySize = total;
	ba->FSBFrequency = bh_tsc_hz();
	/*
	 * SIP off while bringing up, and x86 XNU only honours csrActiveConfig with the flag set. 0x6f is every
	 * CSR_ALLOW_* bit except CSR_ALLOW_APPLE_INTERNAL (0x10), which would make the system behave as an Apple-internal
	 * build (MAC_MACH_UNEXPECTED becomes a panic, IONVRAM internal mode, ...). The one that is required today is
	 * CSR_ALLOW_UNRESTRICTED_FS (0x2): without it the dyld shared cache, which lacks the SIP-protected flag on our
	 * Linux-built volume, is refused by the shared region.
	 */
	ba->flags |= kBootArgsFlagCSRActiveConfig;
	ba->csrActiveConfig = 0x6f;
	ba->KC_hdrs_vaddr = 0;		/* a bare kernel, not a kernel collection */

	err = build_page_tables(ldr.pml4_pa);
	if (err)
		return err;
	err = fill_shared();
	if (err)
		return err;
	bh_logf("xnu: boot_args at %#llx, kaddr %#llx ksize %#x, %d memory ranges (%llu MiB), DT %llu bytes",
		ldr.boot_args_pa, ldr.kaddr, ba->ksize, n, total >> 20, ldr.dt_len);
	bh_logf("xnu: command line: %s", ba->CommandLine);
	return 0;
}
