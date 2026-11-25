// SPDX-License-Identifier: APSL-2.0
/*
 * beringia_xnu.ko: loads an x86_64 XNU kernel into the carve-out beringia_host.ko lends it, builds its boot
 * environment and starts it on a hosted CPU (docs/architecture.md, section 8).
 *
 * Parameters:
 *   kernel=PATH       the kernel Mach-O (default /lib/firmware/beringia/kernel)
 *   boot_args=STRING  XNU's boot arguments
 *   linux_cpu=N       the Linux CPU that runs hosted CPU 0 (default 1)
 *
 * This module uses Linux only through beringia_host.ko's interface (host.h) and the module ABI.
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "loader.h"

static char *kernel = "/lib/firmware/beringia/kernel";
module_param(kernel, charp, 0444);
static char *boot_args = "-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1";
module_param(boot_args, charp, 0444);
static int linux_cpu = 1;
module_param(linux_cpu, int, 0444);

struct ldr ldr;

static int find_low_range(void)
{
	int i;

	ldr.nranges = bh_mem_ranges(ldr.ranges, LDR_MAX_RANGES);
	for (i = 0; i < ldr.nranges; i++) {
		if (ldr.ranges[i].pa + ldr.ranges[i].size <= (4ULL << 30) && ldr.ranges[i].size >= (256ULL << 20) &&
		    !(ldr.ranges[i].pa & ((2ULL << 20) - 1))) {
			ldr.low = ldr.ranges[i];
			return 0;
		}
	}
	bh_log("xnu: no carve-out of 256 MiB or more below 4 GiB (beringia_host.ko carve=...)");
	return -ENOMEM;
}

/* Read the kernel file into the top of the low carve-out (scratch until XNU runs). */
static int read_kernel(u8 **image, u64 *size)
{
	void *f = bh_file_open(kernel, 0, size);
	u64 stage, off;
	u8 *dst;

	if (!f) {
		bh_logf("xnu: cannot open %s", kernel);
		return -ENOENT;
	}
	stage = (ldr.low.pa + ldr.low.size - *size) & ~((2ULL << 20) - 1);
	dst = bh_phys(stage, *size);
	if (!dst || *size > ldr.low.size / 4) {
		bh_file_close(f);
		return -EFBIG;
	}
	for (off = 0; off < *size;) {
		long r = bh_file_rw(f, 0, dst + off, min_t(u64, *size - off, 1 << 20), off);

		if (r <= 0) {
			bh_file_close(f);
			return r < 0 ? r : -EIO;
		}
		off += r;
	}
	bh_file_close(f);
	*image = dst;
	return 0;
}

static int __init ldr_init(void)
{
	struct bh_world_start st;
	u8 *image;
	u64 size;
	int err;

	bh_logf("xnu: Beringia loader, ABI %d, kernel %s", BERINGIA_ABI_VERSION, kernel);
	err = find_low_range();
	if (err)
		return err;
	ldr.slide = ldr.low.pa;
	ldr.ncpus = 1;
	err = read_kernel(&image, &size);
	if (err)
		return err;
	err = ldr_macho_load(image, size);
	if (err)
		return err;
	err = ldr_boot_build(boot_args);
	if (err)
		return err;

	/* CPU 0 enters vstart(boot_args) as pstart would: on low_eintstack, interrupts off */
	st.cr3 = ldr.pml4_pa;
	st.rip = ldr.sym_vstart;
	st.rsp = ldr.sym_stack & ~15ULL;
	st.rdi = ldr.boot_args_pa;
	st.rsi = 0;
	bh_logf("xnu: entering vstart at %#llx (stack %#llx, CR3 %#llx)", st.rip, st.rsp, st.cr3);
	return bh_cpu_start(0, linux_cpu, &st, ldr_hostcall);
}

static void __exit ldr_exit(void)
{
	bh_cpu_stop_all();
	bh_log("xnu: unloaded");
}

module_init(ldr_init);
module_exit(ldr_exit);
MODULE_LICENSE("Proprietary");	/* APSL-2.0 */
MODULE_DESCRIPTION("Beringia: loads and runs a hosted x86_64 XNU");
