// SPDX-License-Identifier: GPL-2.0-only
/*
 * beringia_host.ko: the Linux side of Beringia. Lends a foreign kernel (a hosted x86_64 XNU) memory, CPUs, time,
 * interrupts and files. The interface is host/include/beringia/host.h.
 *
 * Parameters:
 *   carve=BASE:SIZE[,BASE:SIZE...]   physical RAM reserved with memmap= for the foreign kernel
 *   trace=PATH                       also append log lines to this file (survives a VM crash on a drvfs path)
 *   selftest=SECONDS                 run the world-switch self-test at load (no foreign kernel needed)
 *   selftest_cpus=N                  on N CPUs at once (default 1)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/random.h>
#include <linux/timekeeping.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <asm/tsc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include "internal.h"
#include "arch/x86_64/world.h"

static char *carve;
module_param(carve, charp, 0444);
MODULE_PARM_DESC(carve, "BASE:SIZE[,BASE:SIZE...] of RAM reserved with memmap= for the foreign kernel");
static char *trace;
module_param(trace, charp, 0444);
MODULE_PARM_DESC(trace, "append log lines to this file");
static unsigned int selftest;
module_param(selftest, uint, 0444);
MODULE_PARM_DESC(selftest, "run the world-switch self-test for this many seconds at load");
static unsigned int selftest_cpus = 1;
module_param(selftest_cpus, uint, 0444);
static unsigned int selftest_spin = 20000;
module_param(selftest_spin, uint, 0444);
MODULE_PARM_DESC(selftest_spin, "pause iterations between the self-test's host calls");

static struct file *trace_file;
static DEFINE_MUTEX(trace_lock);

/* One line to the kernel log, and to the trace file when there is one (ordinary process context only). */
void bh_log(const char *s)
{
	pr_info("beringia: %s\n", s);
	if (trace_file && !in_interrupt() && !irqs_disabled()) {
		loff_t pos;

		mutex_lock(&trace_lock);
		pos = i_size_read(file_inode(trace_file));
		kernel_write(trace_file, s, strlen(s), &pos);
		kernel_write(trace_file, "\n", 1, &pos);
		mutex_unlock(&trace_lock);
	}
}
EXPORT_SYMBOL(bh_log);

void bh_logf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vscnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bh_log(buf);
}

bh_u64 bh_tsc_hz(void)
{
	return (bh_u64)tsc_khz * 1000;
}
EXPORT_SYMBOL(bh_tsc_hz);

static u64 *bh_linux_pml4(void)
{
	return (u64 *)__va(__read_cr3() & CR3_ADDR_MASK);
}

void bh_linux_pdpt(bh_u64 out[2])
{
	u64 e = bh_linux_pml4()[511];
	u64 *pdpt = (u64 *)__va(e & PTE_PFN_MASK);

	out[0] = pdpt[510];
	out[1] = pdpt[511];
}
EXPORT_SYMBOL(bh_linux_pdpt);

void bh_pml4_used(unsigned char used[512])
{
	u64 *pml4 = bh_linux_pml4();
	int i;

	for (i = 0; i < 512; i++)
		used[i] = i >= 256 && (pml4[i] & _PAGE_PRESENT);
}
EXPORT_SYMBOL(bh_pml4_used);

void *bh_file_open(const char *path, int writable, bh_u64 *size)
{
	struct file *f = filp_open(path, (writable ? O_RDWR : O_RDONLY) | O_LARGEFILE, 0);

	if (IS_ERR(f))
		return NULL;
	if (size)
		*size = i_size_read(file_inode(f));
	return f;
}
EXPORT_SYMBOL(bh_file_open);

long bh_file_rw(void *f, int write, void *buf, unsigned long len, bh_u64 off)
{
	loff_t pos = off;

	return write ? kernel_write(f, buf, len, &pos) : kernel_read(f, buf, len, &pos);
}
EXPORT_SYMBOL(bh_file_rw);

void bh_file_close(void *f)
{
	if (f)
		filp_close(f, NULL);
}
EXPORT_SYMBOL(bh_file_close);

void bh_random(void *buf, unsigned long len)
{
	get_random_bytes(buf, len);
}
EXPORT_SYMBOL(bh_random);

void bh_wallclock(bh_u64 *sec, bh_u64 *nsec)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	*sec = ts.tv_sec;
	*nsec = ts.tv_nsec;
}
EXPORT_SYMBOL(bh_wallclock);

static int __init bh_init(void)
{
	int err;

	if (trace) {
		trace_file = filp_open(trace, O_WRONLY | O_CREAT | O_APPEND | O_LARGEFILE, 0644);
		if (IS_ERR(trace_file))
			trace_file = NULL;
	}
	bh_logf("beringia_host %d loading (tsc %llu Hz)", BH_VERSION, bh_tsc_hz());
	err = bh_world_init();
	if (err)
		goto out;
	err = bh_cpu_init();
	if (err)
		goto out;
	err = bh_mem_init(carve);
	if (err)
		goto out_cpu;
	if (selftest) {
		err = bh_selftest(selftest, selftest_cpus, selftest_spin);
		if (err)
			goto out_mem;
	}
	return 0;
out_mem:
	bh_mem_exit();
out_cpu:
	bh_cpu_exit();
out:
	if (trace_file)
		filp_close(trace_file, NULL);
	return err;
}

static void __exit bh_exit(void)
{
	bh_cpu_stop_all();
	bh_cpu_exit();
	bh_mem_exit();
	bh_log("beringia_host unloaded");
	if (trace_file)
		filp_close(trace_file, NULL);
}

module_init(bh_init);
module_exit(bh_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Beringia: host side of a hosted x86_64 XNU");
