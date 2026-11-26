// SPDX-License-Identifier: GPL-2.0-only
/*
 * The C half of the x86_64 world switch: capturing Linux's CPU state, the initial and transition IDTs, the initial
 * foreign GDT/TSS, and host-call dispatch. See world.S for the switch itself.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/string.h>
#include <asm/desc.h>
#include <asm/msr.h>
#include <asm/fpu/api.h>
#include <asm/special_insns.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>
#include <asm/debugreg.h>
#include <linux/atomic.h>
#include <linux/moduleparam.h>
#include "world.h"
#include "../../internal.h"

struct bh_world bh_worlds[BH_MAX_CPUS];

/*
 * The state safety net. Some CPU state of the Linux world is not switched, because the foreign kernel is not supposed
 * to change it: CR4, CR8 (the task priority), DR7 and a few machine-wide MSRs. It is captured right before every entry
 * into the foreign world and compared right after every return, so a difference is the foreign kernel's doing. It is
 * reported once per register and CPU and restored, except IA32_APIC_BASE: a local APIC that was disabled or switched
 * out of x2APIC mode has lost its configuration, the CPU no longer receives interrupts, and the run has to end.
 */
static unsigned int statecheck = 1;
module_param(statecheck, uint, 0644);
MODULE_PARM_DESC(statecheck, "compare (and restore) Linux's unswitched CPU state after every foreign-world exit");

enum {
	HW_CR4, HW_CR8, HW_DR7, HW_PAT, HW_EFER, HW_APIC_BASE, HW_DEBUGCTL, HW_MTRR_DEF, HW_MISC_ENABLE,
	HW_PERF_GLOBAL, HW_PERF_FIXED, HW_N
};

static const struct {
	const char *name;
	u32 msr;
	bool fatal;
} hw_regs[HW_N] = {
	[HW_CR4]         = { "CR4" },
	[HW_CR8]         = { "CR8" },
	[HW_DR7]         = { "DR7" },
	[HW_PAT]         = { "IA32_PAT", MSR_IA32_CR_PAT },
	[HW_EFER]        = { "EFER", MSR_EFER },
	[HW_APIC_BASE]   = { "IA32_APIC_BASE", MSR_IA32_APICBASE, true },
	[HW_DEBUGCTL]    = { "IA32_DEBUGCTL", MSR_IA32_DEBUGCTLMSR },
	[HW_MTRR_DEF]    = { "IA32_MTRR_DEF_TYPE", MSR_MTRRdefType },
	[HW_MISC_ENABLE] = { "IA32_MISC_ENABLE", MSR_IA32_MISC_ENABLE },
	[HW_PERF_GLOBAL] = { "IA32_PERF_GLOBAL_CTRL", MSR_CORE_PERF_GLOBAL_CTRL },
	[HW_PERF_FIXED]  = { "IA32_FIXED_CTR_CTRL", MSR_CORE_PERF_FIXED_CTR_CTRL },
};

static unsigned int hw_present;		/* bit i: hw_regs[i] can be read on this machine (MSRs probed at load) */
static struct {
	u64 v[HW_N];
	unsigned int reported;
} hw_snap[BH_MAX_CPUS];
static atomic_long_t hw_diffs;

static void hw_probe(void)
{
	u64 x;
	int i;

	hw_present = BIT(HW_CR4) | BIT(HW_CR8) | BIT(HW_DR7);
	for (i = 0; i < HW_N; i++)
		if (hw_regs[i].msr && !rdmsrq_safe(hw_regs[i].msr, &x))
			hw_present |= BIT(i);
}

static void hw_read(u64 *v)
{
	int i;

	v[HW_CR4] = __read_cr4();
	asm volatile("mov %%cr8, %0" : "=r"(v[HW_CR8]));
	v[HW_DR7] = native_get_debugreg(7);
	for (i = 0; i < HW_N; i++)
		if (hw_regs[i].msr && (hw_present & BIT(i)))
			rdmsrq(hw_regs[i].msr, v[i]);
}

static void hw_write(int i, u64 v)
{
	switch (i) {
	case HW_CR4:
		asm volatile("mov %0, %%cr4" : : "r"(v) : "memory");
		break;
	case HW_CR8:
		asm volatile("mov %0, %%cr8" : : "r"(v) : "memory");
		break;
	case HW_DR7:
		native_set_debugreg(7, v);
		break;
	default:
		wrmsrq_safe(hw_regs[i].msr, v);
		break;
	}
}

/* After a foreign-world exit, interrupts off. Returns false if the run cannot continue on this CPU. */
static bool hw_check(struct bh_world *w)
{
	u64 now[HW_N], *was = hw_snap[w->index].v;
	bool ok = true;
	int i;

	hw_read(now);
	for (i = 0; i < HW_N; i++) {
		if (!(hw_present & BIT(i)) || now[i] == was[i])
			continue;
		atomic_long_inc(&hw_diffs);
		if (!(hw_snap[w->index].reported & BIT(i))) {
			hw_snap[w->index].reported |= BIT(i);
			bh_logf("cpu %llu: the foreign kernel changed %s: %#llx -> %#llx (%s)", w->index, hw_regs[i].name,
				was[i], now[i], hw_regs[i].fatal ? "cannot be repaired, ending the run" : "restored");
		}
		if (hw_regs[i].fatal)
			ok = false;
		else
			hw_write(i, was[i]);
	}
	return ok;
}

long bh_world_state_diffs(void)
{
	return atomic_long_read(&hw_diffs);
}

/* 16-byte IDT gate */
struct bh_gate {
	u16 off0, sel;
	u8 ist, type;
	u16 off1;
	u32 off2, zero;
} __packed;

static struct bh_gate bh_boot_idt[256] __aligned(4096);
static struct bh_gate bh_tidt_foreign[256] __aligned(4096);
static struct bh_gate bh_tidt_linux[256] __aligned(4096);
/* lidt operands, used by world.S */
struct desc_ptr bh_tidt_foreign_desc, bh_tidt_linux_desc;

static void set_gate(struct bh_gate *g, void *fn, u16 sel, u8 ist)
{
	u64 a = (u64)fn;

	g->off0 = a & 0xffff;
	g->off1 = (a >> 16) & 0xffff;
	g->off2 = a >> 32;
	g->sel = sel;
	g->ist = ist;
	g->type = 0x8e;		/* present, DPL 0, 64-bit interrupt gate */
	g->zero = 0;
}

static u64 xgetbv0(void)
{
	u32 lo, hi;

	asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((u64)hi << 32) | lo;
}

int bh_world_init(void)
{
	unsigned int eax, ebx, ecx, edx;
	int v;

	if (!boot_cpu_has(X86_FEATURE_XSAVE) || !boot_cpu_has(X86_FEATURE_RDPID) || !boot_cpu_has(X86_FEATURE_FSGSBASE)) {
		pr_err("beringia: this CPU lacks XSAVE, RDPID or FSGSBASE\n");
		return -ENODEV;
	}
	cpuid_count(0xd, 0, &eax, &ebx, &ecx, &edx);
	if (ecx > sizeof(bh_worlds[0].xsave)) {
		pr_err("beringia: XSAVE area of %u bytes does not fit\n", ecx);
		return -ENODEV;
	}
	for (v = 0; v < 256; v++) {
		set_gate(&bh_boot_idt[v], (u8 *)bh_boot_vectors + 16 * v, BHW_F_CS, 0);
		set_gate(&bh_tidt_foreign[v], v == 2 ? (void *)bh_tidt_nmi : (void *)bh_tidt_other, BHW_F_CS, 0);
		set_gate(&bh_tidt_linux[v], v == 2 ? (void *)bh_tidt_nmi : (void *)bh_tidt_other, BHW_L_KCS, 0);
	}
	bh_tidt_foreign_desc.size = sizeof(bh_tidt_foreign) - 1;
	bh_tidt_foreign_desc.address = (unsigned long)bh_tidt_foreign;
	bh_tidt_linux_desc.size = sizeof(bh_tidt_linux) - 1;
	bh_tidt_linux_desc.address = (unsigned long)bh_tidt_linux;
	hw_probe();
	return 0;
}

static inline u16 read_sel(int which)
{
	u16 s = 0;

	switch (which) {
	case 0: asm volatile("mov %%ds, %0" : "=r"(s)); break;
	case 1: asm volatile("mov %%es, %0" : "=r"(s)); break;
	case 2: asm volatile("mov %%fs, %0" : "=r"(s)); break;
	case 3: asm volatile("mov %%gs, %0" : "=r"(s)); break;
	case 4: asm volatile("mov %%ss, %0" : "=r"(s)); break;
	}
	return s;
}

/* Linux's state on this CPU, as it is now: loaded again when the foreign world next hands the CPU back. */
void bh_world_capture_linux(struct bh_world *w)
{
	struct desc_ptr gdt, idt;
	u16 tr;

	w->l_cr3 = __read_cr3();
	native_store_gdt(&gdt);
	store_idt(&idt);
	asm volatile("str %0" : "=r"(tr));
	w->l_gdtr[0] = 0;
	w->l_gdtr[1] = 0;
	memcpy(w->l_gdtr, &gdt, sizeof(gdt));
	w->l_idtr[0] = 0;
	w->l_idtr[1] = 0;
	memcpy(w->l_idtr, &idt, sizeof(idt));
	w->l_tr = tr;
	w->l_gdt_rw = (u64)get_cpu_gdt_rw(smp_processor_id());
	rdmsrq(MSR_GS_BASE, w->l_gs);
	rdmsrq(MSR_KERNEL_GS_BASE, w->l_kgs);
	rdmsrq(MSR_FS_BASE, w->l_fs);
	rdmsrq(MSR_LSTAR, w->l_lstar);
	rdmsrq(MSR_STAR, w->l_star);
	rdmsrq(MSR_CSTAR, w->l_cstar);
	rdmsrq(MSR_SYSCALL_MASK, w->l_sfmask);
	rdmsrq(MSR_TSC_AUX, w->l_tsc_aux);
	rdmsrq(MSR_IA32_SYSENTER_CS, w->l_se_cs);
	rdmsrq(MSR_IA32_SYSENTER_ESP, w->l_se_esp);
	rdmsrq(MSR_IA32_SYSENTER_EIP, w->l_se_eip);
	w->l_xcr0 = xgetbv0();
	w->l_cr0 = read_cr0();
	w->l_sel = read_sel(0) | ((u64)read_sel(1) << 16) | ((u64)read_sel(2) << 32) | ((u64)read_sel(3) << 48);
	w->l_ss = read_sel(4);
	if (statecheck)
		hw_read(hw_snap[w->index].v);
}

/* The initial foreign GDT (XNU's selector layout) and TSS of world w. */
static void bh_world_boot_tables(struct bh_world *w, u64 ist1)
{
	u64 tss = (u64)w->boot_tss;
	u32 limit = 103;
	struct desc_ptr gdtr;

	memset(w->boot_gdt, 0, sizeof(w->boot_gdt));
	memset(w->boot_tss, 0, sizeof(w->boot_tss));
	w->boot_gdt[1] = 0x00af9b000000ffffULL;		/* 0x08: 64-bit code, DPL 0 */
	w->boot_gdt[2] = 0x00cf93000000ffffULL;		/* 0x10: data */
	/* 0x40: 64-bit TSS (available), 16 bytes */
	w->boot_gdt[8] = (limit & 0xffff) | ((tss & 0xffffff) << 16) | (0x89ULL << 40) | (((u64)(limit >> 16) & 0xf) << 48) |
			 (((tss >> 24) & 0xff) << 56);
	w->boot_gdt[9] = tss >> 32;
	*(u64 *)(w->boot_tss + 36) = ist1;		/* IST1 */
	*(u16 *)(w->boot_tss + 102) = 104;		/* no I/O bitmap */
	gdtr.size = sizeof(w->boot_gdt) - 1;
	gdtr.address = (unsigned long)w->boot_gdt;
	w->x_gdtr[0] = 0;
	w->x_gdtr[1] = 0;
	memcpy(w->x_gdtr, &gdtr, sizeof(gdtr));
}

/* The foreign IDT for a CPU's first entry (until the foreign kernel loads its own) and the boot TSS's IST1. */
static struct {
	void *idt;
	unsigned int limit;
	u64 ist1;
} bh_entry[BH_MAX_CPUS];

void bh_world_set_entry_idt(struct bh_world *w, void *idt, unsigned int limit, u64 ist1)
{
	int i = w - bh_worlds;

	bh_entry[i].idt = idt;
	bh_entry[i].limit = limit;
	bh_entry[i].ist1 = ist1;
}

long bh_world_run(int index, u64 cr3, u64 rip, u64 rsp, u64 rdi, u64 rsi,
		  long (*fn)(int, unsigned long, unsigned long, unsigned long))
{
	struct bh_world *w = &bh_worlds[index];
	struct desc_ptr idtr;
	long r;

	memset(w, 0, offsetof(struct bh_world, boot_gdt));
	memset(w->xsave, 0, sizeof(w->xsave));
	*(u16 *)(w->xsave + 0) = 0x37f;		/* FCW: x87 exceptions masked */
	*(u32 *)(w->xsave + 24) = 0x1f80;	/* MXCSR: SSE exceptions masked (loaded even for the init state) */
	w->self = (u64)w;
	w->index = index;
	w->tstack_top = ((u64)w->tstack + sizeof(w->tstack)) & ~15ULL;
	w->fn = (u64)fn;
	w->start_rip = rip;
	w->start_rsp = rsp;
	w->start_rdi = rdi;
	w->start_rsi = rsi;
	bh_world_boot_tables(w, bh_entry[index].ist1);
	idtr.size = bh_entry[index].idt ? bh_entry[index].limit : sizeof(bh_boot_idt) - 1;
	idtr.address = (unsigned long)(bh_entry[index].idt ? bh_entry[index].idt : bh_boot_idt);
	memcpy(w->x_idtr, &idtr, sizeof(idtr));
	w->x_cr3 = cr3;
	w->x_tr = BHW_F_TSS;
	w->x_cs = BHW_F_CS;
	w->x_ss = BHW_F_SS;
	w->x_sel = 0;
	w->x_tsc_aux = BHW_TSC_AUX_MAGIC | index;
	hw_snap[index].reported = 0;

	local_irq_disable();
	preempt_disable();
	kernel_fpu_begin();
	bh_world_capture_linux(w);
	/* what the foreign kernel has not set up yet: Linux's values, except where they would lead into Linux */
	w->x_star = w->l_star;
	w->x_sfmask = w->l_sfmask;
	w->x_xcr0 = w->l_xcr0;
	/*
	 * CR0: the foreign kernel uses SSE in kernel mode, so TS and EM must be clear or its first SSE instruction
	 * raises #NM (Linux runs with TS set for lazy FPU switching; XNU's own #NM path expects a user task).
	 */
	w->x_cr0 = w->l_cr0 & ~(u64)(X86_CR0_TS | X86_CR0_EM);
	r = bh_world_enter(w);
	/* back for good: bh_world_dispatch has already given the FPU back */
	preempt_enable();
	local_irq_enable();
	bh_entry[index].idt = NULL;	/* the next run on this world starts from the boot IDT again */
	bh_entry[index].ist1 = 0;
	return r;
}

/* A host call, in the Linux world on the CPU thread's stack, interrupts off. */
long bh_world_dispatch(struct bh_world *w)
{
	long (*fn)(int, unsigned long, unsigned long, unsigned long) = (void *)w->fn;
	long r;

	kernel_fpu_end();
	/* before anything in Linux can run on a state the foreign kernel changed (interrupts are still off) */
	if (statecheck && !hw_check(w))
		return BH_ABORT_VALUE;
	r = fn((int)w->index, w->nr, w->a1, w->a2);
	local_irq_disable();
	if (r == BH_ABORT_VALUE)
		return r;
	kernel_fpu_begin();
	bh_world_capture_linux(w);
	return r;
}

/* From a handler: Linux's own handler for a hardware vector that arrived in the foreign world. */
void bh_reinject_irq(unsigned int vector)
{
	void (*stub)(void) = (void *)((u8 *)bh_int_table + 8 * (vector & 0xff));

	stub();
}
EXPORT_SYMBOL(bh_reinject_irq);

unsigned long bh_hostcall_entry(void)
{
	return (unsigned long)bh_hostcall_trampoline;
}
EXPORT_SYMBOL(bh_hostcall_entry);
