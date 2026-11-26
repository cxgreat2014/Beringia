/* SPDX-License-Identifier: APSL-2.0 */
/* beringia_xnu.ko internals. */
#ifndef BERINGIA_LOADER_H
#define BERINGIA_LOADER_H

#include <linux/types.h>
#include <beringia/host.h>
#include <beringia/abi.h>

#define LDR_MAX_RANGES 8

struct ldr {
	struct bh_mem_range ranges[LDR_MAX_RANGES];
	int nranges;
	struct bh_mem_range low;        /* the range below 4 GiB that holds the kernel and the boot data */
	u64 slide;                      /* VA and PA slide of the kernel (= low.pa) */
	u64 kaddr, kend;                /* physical extent of the kernel image */
	u64 boot_args_pa, memmap_pa, dt_pa, dt_len, pml4_pa, physfree;
	const char *ramdisk_path;       /* a memory-disk image to boot from (memdev pmd0), or NULL */
	u64 ramdisk_pa, ramdisk_size;   /* where ldr_boot_build placed it (reserved in the memory map) */
	u64 sym_vstart, sym_stack, sym_hook, sym_shared;     /* slid virtual addresses */
	struct beringia_shared *shared; /* host address of XNU's beringia_shared */
	unsigned int ncpus;
};

extern struct ldr ldr;

/* macho.c: load the kernel Mach-O (file image f, n bytes) at its slid addresses; resolve the symbols we need. */
int ldr_macho_load(const u8 *f, u64 n);
/* boot.c */
int ldr_boot_build(const char *cmdline);
/* hostcalls.c */
long ldr_hostcall(int cpu, unsigned long nr, unsigned long a1, unsigned long a2);

/* host address of a kernel-image VA (slid) or physical address; NULL when outside XNU's RAM */
static inline void *ldr_kva(u64 va, u64 len)
{
	return bh_phys(va - 0xffffff8000000000ULL, len);
}

#endif
