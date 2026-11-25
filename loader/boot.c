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

/* /, /chosen, /chosen/memory-map, /efi, /efi/platform */
static u64 build_dt(u8 *base, u64 room)
{
	struct dt_builder b = { base, base, base + room };

	dt_node(&b, 3, 2);
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
			dt_node(&b, 1, 0);
			dt_str(&b, "name", "memory-map");
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
	ba->MemoryMap = ldr.memmap_pa;
	ba->MemoryMapSize = n * sizeof(*mm);
	ba->MemoryMapDescriptorSize = sizeof(*mm);
	ba->MemoryMapDescriptorVersion = 1;
	ba->deviceTreeP = ldr.dt_pa;
	ba->deviceTreeLength = ldr.dt_len;
	ba->kaddr = ldr.kaddr;
	ba->ksize = ldr.physfree - ldr.kaddr;
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
