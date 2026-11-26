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
	if (v)
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

/* /, /chosen, /chosen/memory-map, /efi, /efi/platform */
static u64 build_dt(u8 *base, u64 room)
{
	struct dt_builder b = { base, base, base + room };

	dt_node(&b, 3, 3);
	dt_str(&b, "name", "device-tree");
	dt_str(&b, "compatible", "Beringia1,1");
	dt_str(&b, "model", "Beringia1,1");
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
		dt_node(&b, 1, 1);
		dt_str(&b, "name", "efi");
		{
			dt_node(&b, 3, 0);
			dt_str(&b, "name", "platform");
			dt_u64(&b, "FSBFrequency", bh_tsc_hz());
			dt_u64(&b, "ARTFrequency", 0);
		}
	}
	{
		/* /options: IODTPlatformExpert::processTopLevel requires it (gIOOptionsEntry, the NVRAM node) */
		dt_node(&b, 2, 0);
		dt_str(&b, "name", "options");
		dt_str(&b, "boot-args", "");
	}
	return b.p - b.base;
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
	if (pa + size > ldr.low.pa + ldr.low.size) {
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
	strscpy(ba->CommandLine, cmdline, sizeof(ba->CommandLine));
	/* BeringiaDisk uses the exported PE_parse_boot_argn KPI instead of importing the kernel's non-KPI beringia_call.
	 * This is exactly the entry address fill_shared writes to beringia_hook, in the host-module mapping shared with XNU. */
	{
		size_t used = strlen(ba->CommandLine);
		int added = snprintf(ba->CommandLine + used, sizeof(ba->CommandLine) - used,
		    " %s=%#llx", BERINGIA_HOSTCALL_BOOT_ARG, (unsigned long long)bh_hostcall_entry());

		if (added < 0 || (size_t)added >= sizeof(ba->CommandLine) - used)
			return -E2BIG;
	}
	if (ldr.ramdisk_size)
		scnprintf(ba->CommandLine + strlen(ba->CommandLine),
		    sizeof(ba->CommandLine) - strlen(ba->CommandLine),
		    " pmd0=%#llx.%#llx rd=md0", ldr.ramdisk_pa, ldr.ramdisk_size);
	ba->MemoryMap = ldr.memmap_pa;
	ba->MemoryMapSize = n * sizeof(*mm);
	ba->MemoryMapDescriptorSize = sizeof(*mm);
	ba->MemoryMapDescriptorVersion = 1;
	ba->deviceTreeP = ldr.dt_pa;
	ba->deviceTreeLength = ldr.dt_len;
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
	ba->csrActiveConfig = 0x7f;	/* SIP off while bringing up (CSR_ALLOW_* bits) */
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
