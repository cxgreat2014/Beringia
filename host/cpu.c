// SPDX-License-Identifier: GPL-2.0-only
/*
 * Foreign CPUs: one kernel thread per foreign CPU, bound to a Linux CPU; idle waits, kicks, one-shot timers, yields.
 * (docs/architecture.md, sections 5 and 6)
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <asm/tsc.h>
#include "internal.h"
#include "arch/x86_64/world.h"

struct bh_cpu {
	struct task_struct *thread;
	struct completion done;
	struct bh_world_start st;
	bh_hostcall_fn fn;
	int linux_cpu;
	int running;
	int stop;
	int kicked;
	struct hrtimer timer;
	void (*fire)(int cpu);
	long result;
};

static struct bh_cpu cpus[BH_MAX_CPUS];

static u64 tsc_to_ns(u64 cycles)
{
	return mul_u64_u32_div(cycles, 1000000, tsc_khz);
}

static int bh_cpu_thread(void *arg)
{
	struct bh_cpu *c = arg;
	int index = c - cpus;

	c->result = bh_world_run(index, c->st.cr3, c->st.rip, c->st.rsp, c->st.rdi, c->st.rsi, c->fn);
	bh_logf("cpu %d: left the foreign world (%ld)", index, c->result);
	c->running = 0;
	complete(&c->done);
	return 0;
}

int bh_cpu_start(int cpu, int linux_cpu, const struct bh_world_start *st, bh_hostcall_fn fn)
{
	struct bh_cpu *c;
	struct task_struct *t;

	if (cpu < 0 || cpu >= BH_MAX_CPUS || !cpu_online(linux_cpu))
		return -EINVAL;
	c = &cpus[cpu];
	if (c->running)
		return -EBUSY;
	c->st = *st;
	c->fn = fn;
	c->linux_cpu = linux_cpu;
	c->stop = 0;
	c->kicked = 0;
	init_completion(&c->done);
	t = kthread_create(bh_cpu_thread, c, "beringia/%d", cpu);
	if (IS_ERR(t))
		return PTR_ERR(t);
	kthread_bind(t, linux_cpu);
	c->thread = t;
	c->running = 1;
	wake_up_process(t);
	return 0;
}
EXPORT_SYMBOL(bh_cpu_start);

int bh_stop_requested(int cpu)
{
	return READ_ONCE(cpus[cpu].stop);
}
EXPORT_SYMBOL(bh_stop_requested);

void bh_cpu_stop_all(void)
{
	int i;

	for (i = 0; i < BH_MAX_CPUS; i++) {
		if (!cpus[i].running)
			continue;
		WRITE_ONCE(cpus[i].stop, 1);
		bh_kick(i);
	}
	for (i = 0; i < BH_MAX_CPUS; i++) {
		if (!cpus[i].thread)
			continue;
		while (!wait_for_completion_timeout(&cpus[i].done, 10 * HZ))
			bh_logf("cpu %d: still in the foreign world, waiting", i);
		cpus[i].thread = NULL;
	}
}
EXPORT_SYMBOL(bh_cpu_stop_all);

void bh_yield(void)
{
	preempt_enable();
	local_irq_enable();
	cond_resched();
	local_irq_disable();
	preempt_disable();
}
EXPORT_SYMBOL(bh_yield);

void bh_kick(int cpu)
{
	struct bh_cpu *c = &cpus[cpu];

	WRITE_ONCE(c->kicked, 1);
	if (c->thread)
		wake_up_process(c->thread);
}
EXPORT_SYMBOL(bh_kick);

void bh_idle(int cpu, bh_u64 tsc_deadline, const volatile unsigned int *wake)
{
	struct bh_cpu *c = &cpus[cpu];

	preempt_enable();
	local_irq_enable();
	for (;;) {
		u64 now = rdtsc();
		ktime_t t;

		set_current_state(TASK_INTERRUPTIBLE);
		if (READ_ONCE(c->kicked) || READ_ONCE(c->stop) || (wake && *wake) || (tsc_deadline && now >= tsc_deadline))
			break;
		/*
		 * Bounded wait: never park this CPU indefinitely. The foreign kernel idles through us and, especially
		 * before its timer is wired up, nothing would kick it — it would stall here with interrupts off.
		 * Wake within ~4 ms so it can make progress and check for pending work.
		 */
		if (tsc_deadline && now < tsc_deadline) {
			t = ns_to_ktime(tsc_to_ns(tsc_deadline - now));
			schedule_hrtimeout_range(&t, 20000, HRTIMER_MODE_REL);
		} else if (!tsc_deadline) {
			t = ns_to_ktime(tsc_to_ns(tsc_khz * 4ULL));	/* tsc_khz cycles = 1 ms */
			schedule_hrtimeout_range(&t, 20000, HRTIMER_MODE_REL);
			break;	/* bounded wait elapsed: return to the foreign kernel */
		} else {
			break;	/* deadline passed */
		}
	}
	__set_current_state(TASK_RUNNING);
	WRITE_ONCE(c->kicked, 0);
	local_irq_disable();
	preempt_disable();
}
EXPORT_SYMBOL(bh_idle);

static enum hrtimer_restart bh_timer_fn(struct hrtimer *t)
{
	struct bh_cpu *c = container_of(t, struct bh_cpu, timer);
	int cpu = c - cpus;
	void (*fire)(int) = READ_ONCE(c->fire);

	if (fire)
		fire(cpu);
	bh_kick(cpu);
	return HRTIMER_NORESTART;
}

void bh_timer_set(int cpu, bh_u64 tsc_deadline, void (*fire)(int cpu))
{
	struct bh_cpu *c = &cpus[cpu];
	u64 now = rdtsc();

	hrtimer_try_to_cancel(&c->timer);
	if (!tsc_deadline)
		return;
	WRITE_ONCE(c->fire, fire);
	hrtimer_start(&c->timer, ns_to_ktime(tsc_deadline > now ? tsc_to_ns(tsc_deadline - now) : 0),
		      HRTIMER_MODE_REL_PINNED_HARD);
}
EXPORT_SYMBOL(bh_timer_set);

int bh_cpu_init(void)
{
	int i;

	for (i = 0; i < BH_MAX_CPUS; i++)
		hrtimer_setup(&cpus[i].timer, bh_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	return 0;
}

void bh_cpu_exit(void)
{
	int i;

	for (i = 0; i < BH_MAX_CPUS; i++)
		hrtimer_cancel(&cpus[i].timer);
}
