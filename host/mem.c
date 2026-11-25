// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve-outs: physical RAM that Linux was told not to use (memmap=SIZE$BASE) and that the foreign kernel manages as its
 * own (ADR 0001). Claimed in /proc/iomem as "beringia" and mapped write-back for host-side access.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include "internal.h"

#define BH_MAX_RANGES 8

static struct {
	u64 pa, size;
	void *va;
	struct resource *res;
} ranges[BH_MAX_RANGES];
static int nranges;

static int claim(u64 pa, u64 size)
{
	if (nranges == BH_MAX_RANGES)
		return -ENOSPC;
	if (!size || (pa | size) & (SZ_2M - 1)) {
		bh_logf("carve-out %#llx+%#llx must be 2 MiB aligned", pa, size);
		return -EINVAL;
	}
	if (region_intersects(pa, size, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) != REGION_DISJOINT) {
		bh_logf("carve-out %#llx+%#llx overlaps Linux's RAM (reserve it with memmap=SIZE$BASE)", pa, size);
		return -EBUSY;
	}
	ranges[nranges].res = request_mem_region(pa, size, "beringia");
	if (!ranges[nranges].res) {
		bh_logf("carve-out %#llx+%#llx is in use", pa, size);
		return -EBUSY;
	}
	ranges[nranges].va = memremap(pa, size, MEMREMAP_WB);
	if (!ranges[nranges].va) {
		release_mem_region(pa, size);
		bh_logf("carve-out %#llx+%#llx cannot be mapped", pa, size);
		return -ENOMEM;
	}
	ranges[nranges].pa = pa;
	ranges[nranges].size = size;
	nranges++;
	bh_logf("carve-out %#llx-%#llx (%llu MiB)", pa, pa + size, size >> 20);
	return 0;
}

int bh_mem_init(const char *spec)
{
	char *s, *p, *tok;
	int err = 0;

	if (!spec || !*spec)
		return 0;
	s = kstrdup(spec, GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	p = s;
	while ((tok = strsep(&p, ",")) && !err) {
		char *colon = strchr(tok, ':');
		u64 pa, size;

		if (!colon) {
			err = -EINVAL;
			break;
		}
		*colon = 0;
		err = kstrtoull(tok, 0, &pa) ?: kstrtoull(colon + 1, 0, &size);
		if (!err)
			err = claim(pa, size);
	}
	kfree(s);
	if (err)
		bh_mem_exit();
	return err;
}

void bh_mem_exit(void)
{
	while (nranges) {
		nranges--;
		memunmap(ranges[nranges].va);
		release_mem_region(ranges[nranges].pa, ranges[nranges].size);
	}
}

int bh_mem_ranges(struct bh_mem_range *r, int max)
{
	int i;

	for (i = 0; i < nranges && i < max; i++) {
		r[i].pa = ranges[i].pa;
		r[i].size = ranges[i].size;
	}
	return nranges;
}
EXPORT_SYMBOL(bh_mem_ranges);

void *bh_phys(bh_u64 pa, bh_u64 len)
{
	int i;

	for (i = 0; i < nranges; i++)
		if (pa >= ranges[i].pa && pa + len >= pa && pa + len <= ranges[i].pa + ranges[i].size)
			return (u8 *)ranges[i].va + (pa - ranges[i].pa);
	return NULL;
}
EXPORT_SYMBOL(bh_phys);
