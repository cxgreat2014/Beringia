// SPDX-License-Identifier: APSL-2.0
/*
 * Host calls from the hosted XNU (enum beringia_call, xnu/include/beringia/abi.h). Each runs in the Linux world on the
 * calling CPU's thread, interrupts off.
 */
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include "loader.h"

#define BH_HC_TRAP 0xfff0	/* the host's initial IDT: an exception before XNU's own IDT was loaded */

static unsigned long fwd_count, tmr_count;
static DEFINE_MUTEX(block_io_lock);

/* Host calls enter with preemption and interrupts off. Positional file I/O may sleep, so run just that section as
 * ordinary Linux kernel-thread context and restore the world-switch invariant before returning. */
static void block_io_enter(void)
{
	preempt_enable();
	local_irq_enable();
	mutex_lock(&block_io_lock);
}

static void block_io_leave(void)
{
	mutex_unlock(&block_io_lock);
	local_irq_disable();
	preempt_disable();
}

static long ldr_block_info(unsigned long a1)
{
	struct beringia_block_info *info = bh_phys(a1, sizeof(*info));
	unsigned int unit;

	if (!info)
		return -EFAULT;
	unit = info->unit;
	memset(info, 0, sizeof(*info));
	info->unit = unit;
	info->block_size = BERINGIA_BLOCK_SIZE;
	if (unit == 0 && ldr.disk_file) {
		info->size = ldr.disk_size;
		info->read_only = ldr.disk_read_only;
	}
	return 0;
}

/* The request's result word, the only field the host writes back into XNU's copy. */
static long ldr_block_result(struct beringia_block_req *shared, long long result)
{
	WRITE_ONCE(shared->result, result);
	return 0;
}

static long ldr_block_rw(unsigned long a1)
{
	struct beringia_block_req *shared = bh_phys(a1, sizeof(*shared));
	struct beringia_block_req req;
	void *segment[BERINGIA_BLOCK_MAX_SEGS];
	u64 total = 0, done = 0;
	unsigned int i;
	long error = 0;

	if (!shared)
		return -EFAULT;
	/*
	 * One copy, then only the copy: the request lives in XNU's memory, and with more than one hosted CPU another one
	 * could change it between the checks below and the I/O (a larger nseg or len, a NULL segment made non-empty).
	 */
	memcpy(&req, shared, sizeof(req));
	barrier();
	if (req.unit != 0 || !ldr.disk_file)
		return ldr_block_result(shared, -ENODEV);
	if (req.write == BERINGIA_BLOCK_FLUSH) {
		if (req.nseg != 0)
			return ldr_block_result(shared, -EINVAL);
		/* After every write that completed before it (the lock orders it after a concurrent one), to stable
		 * storage: the disk reports a write cache, so APFS relies on this for its checkpoints. */
		block_io_enter();
		error = bh_file_sync(ldr.disk_file);
		block_io_leave();
		return ldr_block_result(shared, error);
	}
	if (req.write != BERINGIA_BLOCK_READ && req.write != BERINGIA_BLOCK_WRITE)
		return ldr_block_result(shared, -EINVAL);
	if (req.nseg > BERINGIA_BLOCK_MAX_SEGS)
		return ldr_block_result(shared, -EINVAL);
	if (req.write == BERINGIA_BLOCK_WRITE && ldr.disk_read_only)
		return ldr_block_result(shared, -EROFS);
	if (req.offset > ldr.disk_size)
		return ldr_block_result(shared, -EINVAL);

	/* Validate the entire request before a write can have a partial side effect. */
	for (i = 0; i < req.nseg; i++) {
		u64 len = req.seg[i].len;

		segment[i] = NULL;
		if (!len)
			continue;
		if (total > ldr.disk_size - req.offset ||
		    len > ldr.disk_size - req.offset - total ||
		    req.seg[i].pa + len < req.seg[i].pa)
			return ldr_block_result(shared, -EINVAL);
		segment[i] = bh_phys(req.seg[i].pa, len);
		if (!segment[i])
			return ldr_block_result(shared, -EFAULT);
		total += len;
	}

	block_io_enter();
	for (i = 0; i < req.nseg && !error; i++) {
		u64 len = req.seg[i].len, at = 0;

		while (at < len) {
			long n = bh_file_rw(ldr.disk_file, req.write,
			    (u8 *)segment[i] + at, len - at, req.offset + done);

			if (n <= 0 || (u64)n > len - at) {
				error = n < 0 ? n : -EIO;
				break;
			}
			at += n;
			done += n;
		}
	}
	block_io_leave();
	return ldr_block_result(shared, error ? error : (long long)done);
}

static void ldr_timer_fire(int cpu)
{
	static unsigned long fire_count;

	/* atomic: XNU takes the word with __atomic_fetch_and, other CPUs set IPI bits concurrently */
	__atomic_fetch_or(&ldr.shared->cpu[cpu].pending, BERINGIA_PENDING_TIMER, __ATOMIC_SEQ_CST);
	if (++fire_count <= 10)
		bh_logf("xnu: host timer fire %lu cpu %d", fire_count, cpu);
}

/* per hosted CPU: only that CPU's thread runs its host calls (static, not on the stack: 2 KiB each) */
static char console_buf[BERINGIA_MAX_CPUS][sizeof(((struct beringia_cpu_shared *)0)->console) + 1];
static char console_full[BERINGIA_MAX_CPUS][sizeof(console_buf[0]) + 16];

static void console_line(int cpu, unsigned long len, const char *prefix)
{
	char *buf = console_buf[cpu];

	len = min_t(unsigned long, len, sizeof(console_buf[0]) - 1);
	memcpy(buf, ldr.shared->cpu[cpu].console, len);
	buf[len] = 0;
	snprintf(console_full[cpu], sizeof(console_full[0]), "%s%d: %s", prefix, cpu, buf);
	bh_console(console_full[cpu]);
}

long ldr_hostcall(int cpu, unsigned long nr, unsigned long a1, unsigned long a2)
{
	static unsigned long hc_count;

	hc_count++;
	if (hc_count <= 40)
		bh_logf("xnu: hc %lu nr=%#lx", hc_count, nr);
	switch (nr) {
	case BERINGIA_CALL_CONSOLE:
		if (a1 < BERINGIA_MAX_CPUS)
			console_line(a1, a2, "xnu");
		return 0;
	case BERINGIA_CALL_PANIC:
		if (a1 < BERINGIA_MAX_CPUS)
			console_line(a1, a2, "XNU PANIC on cpu ");
		return BH_ABORT;
	case BERINGIA_CALL_HALT:
		bh_logf("xnu: halt (%lu)", a1);
		return BH_ABORT;
	case BERINGIA_CALL_MARK:
		bh_logf("xnu: mark %lu %#lx", a1, a2);
		return 0;
	case BERINGIA_CALL_FORWARD_IRQ:
		fwd_count++;
		if (fwd_count % 100 == 0 || fwd_count <= 5)
			bh_logf("xnu: forwarded %lu irqs (vec %lu)", fwd_count, a1);
		bh_reinject_irq(a1);
		return bh_stop_requested(cpu) ? BH_ABORT : 0;
	case BERINGIA_CALL_SET_TIMER:
		tmr_count++;
		if (tmr_count <= 5 || tmr_count % 100 == 0)
			bh_logf("xnu: set_timer %lu (deadline %#lx)", tmr_count, a1);
		ldr.shared->cpu[cpu].timer_deadline = a1;
		bh_timer_set(cpu, a1, ldr_timer_fire);
		return 0;
	case BERINGIA_CALL_IDLE: {
		static unsigned long idle_count;

		if (++idle_count <= 10 || idle_count % 1000 == 0)
			bh_logf("xnu: idle %lu (deadline %#lx, pending %#x)", idle_count, a1, ldr.shared->cpu[cpu].pending);
		bh_idle(cpu, a1, &ldr.shared->cpu[cpu].pending);
		if (idle_count <= 10)
			bh_logf("xnu: idle return %lu (pending %#x)", idle_count, ldr.shared->cpu[cpu].pending);
		return bh_stop_requested(cpu) ? BH_ABORT : 0;
	}
	case BERINGIA_CALL_IPI:
		if (a1 < BERINGIA_MAX_CPUS)
			bh_kick(a1);
		return 0;
	case BERINGIA_CALL_YIELD:
		bh_yield();
		return bh_stop_requested(cpu) ? BH_ABORT : 0;
	case BERINGIA_CALL_WALLCLOCK: {
		bh_u64 *t = bh_phys(a1, 16);

		if (!t)
			return -EFAULT;
		bh_wallclock(&t[0], &t[1]);
		return 0;
	}
	case BERINGIA_CALL_RANDOM: {
		void *p = bh_phys(a1, a2);

		if (!p || a2 > 4096)
			return -EFAULT;
		bh_random(p, a2);
		return 0;
	}
	case BERINGIA_CALL_BLOCK_INFO:
		return ldr_block_info(a1);
	case BERINGIA_CALL_BLOCK_RW:
		return ldr_block_rw(a1);
	case BH_HC_TRAP:
		bh_logf("xnu: exception %lu at %#lx before XNU had its own IDT", a1, a2);
		return BH_ABORT;
	default:
		bh_logf("xnu: unknown host call %lu (%#lx, %#lx)", nr, a1, a2);
		return -ENOSYS;
	}
}
