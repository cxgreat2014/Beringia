// SPDX-License-Identifier: APSL-2.0
/*
 * Host calls from the hosted XNU (enum beringia_call, xnu/include/beringia/abi.h). Each runs in the Linux world on the
 * calling CPU's thread, interrupts off.
 */
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include "loader.h"

#define BH_HC_TRAP 0xfff0	/* the host's initial IDT: an exception before XNU's own IDT was loaded */

static void ldr_timer_fire(int cpu)
{
	ldr.shared->cpu[cpu].pending |= BERINGIA_PENDING_TIMER;
}

static char console_buf[BERINGIA_MAX_CPUS][sizeof(((struct beringia_cpu_shared *)0)->console) + 1];

static void console_line(int cpu, unsigned long len, const char *prefix)
{
	char *buf = console_buf[cpu];

	len = min_t(unsigned long, len, sizeof(console_buf[0]) - 1);
	memcpy(buf, ldr.shared->cpu[cpu].console, len);
	buf[len] = 0;
	{
		char full[sizeof(console_buf[0]) + 16];

		snprintf(full, sizeof(full), "%s%d: %s", prefix, cpu, buf);
		bh_console(full);
	}
}

long ldr_hostcall(int cpu, unsigned long nr, unsigned long a1, unsigned long a2)
{
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
		bh_reinject_irq(a1);
		return bh_stop_requested(cpu) ? BH_ABORT : 0;
	case BERINGIA_CALL_SET_TIMER:
		ldr.shared->cpu[cpu].timer_deadline = a1;
		bh_timer_set(cpu, a1, ldr_timer_fire);
		return 0;
	case BERINGIA_CALL_IDLE:
		bh_idle(cpu, a1, &ldr.shared->cpu[cpu].pending);
		return bh_stop_requested(cpu) ? BH_ABORT : 0;
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
	case BH_HC_TRAP:
		bh_logf("xnu: exception %lu at %#lx before XNU had its own IDT", a1, a2);
		return BH_ABORT;
	default:
		bh_logf("xnu: unknown host call %lu (%#lx, %#lx)", nr, a1, a2);
		return -ENOSYS;
	}
}
