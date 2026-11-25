// SPDX-License-Identifier: GPL-2.0-only
/*
 * World-switch self-test (ROADMAP M1): run the tiny foreign kernel of selftest.S on N CPUs for a while and check that
 * it ran, took its own exceptions, and that Linux's interrupts arriving in its world were forwarded and handled.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <asm/tlbflush.h>
#include "internal.h"
#include "arch/x86_64/world.h"

#define ST_MAX 8

void bh_st_main(void);
void bh_st_ud(void);
void bh_st_vectors(void);
extern u64 bh_st_loops, bh_st_uds;

struct bh_gate16 {
	u16 off0, sel;
	u8 ist, type;
	u16 off1;
	u32 off2, zero;
} __packed;

static struct bh_gate16 st_idt[256] __aligned(4096);
static u8 st_stack[ST_MAX][16384] __aligned(4096);
static u8 st_ist[ST_MAX][8192] __aligned(4096);
static atomic_long_t st_irqs, st_yields, st_vec_count[256];
static ktime_t st_end;
static int st_failed;

static void st_gate(int v, void *fn, u8 ist)
{
	u64 a = (u64)fn;

	st_idt[v].off0 = a & 0xffff;
	st_idt[v].off1 = (a >> 16) & 0xffff;
	st_idt[v].off2 = a >> 32;
	st_idt[v].sel = BHW_F_CS;
	st_idt[v].ist = ist;
	st_idt[v].type = 0x8e;
	st_idt[v].zero = 0;
}

static long st_hostcall(int cpu, unsigned long nr, unsigned long a1, unsigned long a2)
{
	switch (nr) {
	case 16:	/* forwarded interrupt */
		atomic_long_inc(&st_irqs);
		atomic_long_inc(&st_vec_count[a1 & 0xff]);
		bh_reinject_irq(a1);
		return 0;
	case 21:	/* yield */
		atomic_long_inc(&st_yields);
		if (bh_stop_requested(cpu) || ktime_after(ktime_get(), st_end))
			return BH_ABORT;
		bh_yield();
		return 0;
	case 0xfff0:	/* an exception the test kernel did not expect */
		bh_logf("selftest cpu %d: unexpected vector %lu at %#lx", cpu, a1, a2);
		st_failed = 1;
		return BH_ABORT;
	default:
		bh_logf("selftest cpu %d: unknown host call %lu", cpu, nr);
		st_failed = 1;
		return BH_ABORT;
	}
}

int bh_selftest(unsigned int seconds, unsigned int ncpus, unsigned int spin)
{
	unsigned long pml4[ST_MAX] = { 0 };
	int i, v, started = 0, err = 0;
	long irqs, yields;

	if (ncpus < 1 || ncpus > ST_MAX || ncpus >= num_online_cpus())
		return -EINVAL;
	for (v = 0; v < 32; v++)
		st_gate(v, (u8 *)bh_boot_vectors + 16 * v, 0);
	st_gate(6, bh_st_ud, 0);
	for (v = 32; v < 256; v++)
		st_gate(v, (u8 *)bh_st_vectors + 16 * (v - 32), 1);
	bh_st_loops = 0;
	bh_st_uds = 0;
	atomic_long_set(&st_irqs, 0);
	atomic_long_set(&st_yields, 0);
	for (v = 0; v < 256; v++)
		atomic_long_set(&st_vec_count[v], 0);
	st_failed = 0;
	st_end = ktime_add_ms(ktime_get(), seconds * 1000ULL);
	bh_logf("selftest: %u CPU(s), %u s, %u spins per host call", ncpus, seconds, spin);

	for (i = 0; i < ncpus; i++) {
		struct bh_world_start st;

		/* a CR3 of its own: a copy of Linux's top-level table (so the module stays mapped) */
		pml4[i] = get_zeroed_page(GFP_KERNEL);
		if (!pml4[i]) {
			err = -ENOMEM;
			break;
		}
		memcpy((void *)pml4[i], __va(__read_cr3() & CR3_ADDR_MASK), PAGE_SIZE);
		bh_world_set_entry_idt(&bh_worlds[i], st_idt, sizeof(st_idt) - 1, (u64)(st_ist[i] + sizeof(st_ist[i])));
		st.cr3 = __pa(pml4[i]);
		st.rip = (u64)bh_st_main;
		st.rsp = (u64)(st_stack[i] + sizeof(st_stack[i]));
		st.rdi = spin ?: 1;
		st.rsi = 0;
		/* Linux CPUs 1..n: CPU 0 keeps running the rest of the system */
		err = bh_cpu_start(i, i + 1, &st, st_hostcall);
		if (err)
			break;
		started++;
	}
	if (!err)
		msleep(seconds * 1000 + 200);
	bh_cpu_stop_all();
	for (i = 0; i < ncpus; i++)
		if (pml4[i])
			free_page(pml4[i]);

	irqs = atomic_long_read(&st_irqs);
	yields = atomic_long_read(&st_yields);
	bh_logf("selftest: loops %llu, #UD handled %llu, host calls (yield) %ld, forwarded interrupts %ld",
		bh_st_loops, bh_st_uds, yields, irqs);
	for (v = 0; v < 256; v++)
		if (atomic_long_read(&st_vec_count[v]))
			bh_logf("selftest:   vector %#x: %ld", v, atomic_long_read(&st_vec_count[v]));
	if (err || st_failed || !bh_st_loops || bh_st_uds < bh_st_loops || !yields) {
		bh_logf("selftest: FAILED (err %d)", err);
		return err ?: -EIO;
	}
	bh_logf("selftest: PASSED");
	return 0;
}
