/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * x86_64 world switch between Linux and a foreign kernel (docs/architecture.md, section 3; ADR 0002).
 *
 * The Linux world and the foreign world each have their own CR3, GDT/TR, IDT, CS/SS and data selectors, FS/GS bases,
 * syscall MSRs, TSC_AUX, XCR0, CR0 and XSAVE state. Switching order is chosen so that at every instruction the stack
 * and the code in use are mapped in the active page tables (the per-CPU transition stack and this code live in module
 * memory, which both worlds map), and the IDT is a transition IDT (world.c) whose gates are valid in the active GDT,
 * except for one or two instructions around each descriptor-table load. An NMI in those windows is the residual risk.
 *
 * r15 holds the struct bh_world pointer throughout. In the foreign world TSC_AUX = BHW_TSC_AUX_MAGIC | index, which
 * the host-call trampoline reads with rdpid to find its world without touching memory first.
 *
 * Code that runs in the foreign world must not reach Linux's call-depth tracking or return thunks (they update
 * %gs-relative counters, and %gs is the foreign kernel's there): it uses SYM_CODE and plain ret, and this object is
 * excluded from objtool (OBJECT_FILES_NON_STANDARD), so no call site in it is rewritten into a call thunk.
 */
#include <linux/linkage.h>
#include <linux/objtool.h>
#include <asm/unwind_hints.h>
#include <asm/nospec-branch.h>
#include "world.h"

#define MSR_FS_BASE         0xc0000100
#define MSR_GS_BASE         0xc0000101
#define MSR_KGS_BASE        0xc0000102
#define MSR_STAR            0xc0000081
#define MSR_LSTAR           0xc0000082
#define MSR_CSTAR           0xc0000083
#define MSR_SFMASK          0xc0000084
#define MSR_TSC_AUX         0xc0000103
#define MSR_SE_CS           0x174
#define MSR_SE_ESP          0x175
#define MSR_SE_EIP          0x176
#define CR0_WP              (1 << 16)
#define BH_HC_TRAP          0xfff0      /* host call made by the boot IDT: a1 = vector, a2 = rip */

.macro RDMSR_TO msr, off
	movl	$\msr, %ecx
	rdmsr
	shlq	$32, %rdx
	orq	%rdx, %rax
	movq	%rax, \off(%r15)
.endm

.macro WRMSR_FROM msr, off
	movl	$\msr, %ecx
	movq	\off(%r15), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	wrmsr
.endm

/* Clear the busy bit of the TSS descriptor for selector \tr in the GDT at \base (registers), with CR0.WP off. */
.macro TSS_AVAILABLE base, tr
	andq	$0xfff8, \tr
	addq	\tr, \base
	movq	%cr0, %rcx
	movq	%rcx, %rdx
	andq	$~CR0_WP, %rdx
	movq	%rdx, %cr0
	andb	$0xfd, 5(\base)
	movq	%rcx, %cr0
.endm

/* Reload CS with \sel through a far return. */
.macro LOAD_CS sel
	pushq	$\sel
	leaq	1f(%rip), %rax
	pushq	%rax
	lretq
1:
.endm

	.text

/*
 * Switch from the foreign world (state already saved) to the Linux world. In: r15 = world. Out: Linux's CR3,
 * descriptor tables, selectors, MSRs, XCR0, CR0; rsp on the transition stack.
 */
SYM_CODE_START_LOCAL(to_linux)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	popq	%r14				/* return address: the stack is about to change */
	movq	BHW_TSTACK_TOP(%r15), %rsp
	lidt	bh_tidt_foreign_desc(%rip)
	movq	BHW_L_CR3(%r15), %rax
	movq	%rax, %cr3
	/* GDT and TR: through Linux's writable GDT alias (the loaded one is a read-only fixmap) */
	subq	$16, %rsp
	movw	BHW_L_GDTR(%r15), %ax
	movw	%ax, (%rsp)
	movq	BHW_L_GDT_RW(%r15), %rax
	movq	%rax, 2(%rsp)
	lgdt	(%rsp)
	lidt	bh_tidt_linux_desc(%rip)
	addq	$16, %rsp
	movq	BHW_L_GDT_RW(%r15), %rsi
	movq	BHW_L_TR(%r15), %rdi
	TSS_AVAILABLE %rsi, %rdi
	movq	BHW_L_TR(%r15), %rax
	ltr	%ax
	lgdt	BHW_L_GDTR(%r15)
	LOAD_CS	BHW_L_KCS
	movq	BHW_L_SS(%r15), %rax
	movw	%ax, %ss
	movq	BHW_L_SEL(%r15), %rax
	movw	%ax, %ds
	shrq	$16, %rax
	movw	%ax, %es
	shrq	$16, %rax
	movw	%ax, %fs
	shrq	$16, %rax
	movw	%ax, %gs
	WRMSR_FROM MSR_FS_BASE, BHW_L_FS
	WRMSR_FROM MSR_GS_BASE, BHW_L_GS
	WRMSR_FROM MSR_KGS_BASE, BHW_L_KGS
	WRMSR_FROM MSR_LSTAR, BHW_L_LSTAR
	WRMSR_FROM MSR_STAR, BHW_L_STAR
	WRMSR_FROM MSR_CSTAR, BHW_L_CSTAR
	WRMSR_FROM MSR_SFMASK, BHW_L_SFMASK
	WRMSR_FROM MSR_SE_CS, BHW_L_SE_CS
	WRMSR_FROM MSR_SE_ESP, BHW_L_SE_ESP
	WRMSR_FROM MSR_SE_EIP, BHW_L_SE_EIP
	WRMSR_FROM MSR_TSC_AUX, BHW_L_TSC_AUX
	movq	BHW_L_XCR0(%r15), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xorl	%ecx, %ecx
	xsetbv
	movq	BHW_L_CR0(%r15), %rax
	movq	%rax, %cr0
	lidt	BHW_L_IDTR(%r15)
	movq	$0, BHW_IN_FOREIGN(%r15)
	ANNOTATE_RETPOLINE_SAFE
	jmp	*%r14
SYM_CODE_END(to_linux)

/*
 * Switch from the Linux world to the foreign world. In: r15 = world, with the x_* fields to load. Out: the foreign
 * world's state loaded (FPU included); rsp on the transition stack.
 */
SYM_CODE_START_LOCAL(to_foreign)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	popq	%r14
	movq	BHW_TSTACK_TOP(%r15), %rsp
	lidt	bh_tidt_linux_desc(%rip)
	movq	BHW_X_CR3(%r15), %rax
	movq	%rax, %cr3
	lgdt	BHW_X_GDTR(%r15)
	lidt	bh_tidt_foreign_desc(%rip)
	movq	BHW_X_GDTR+2(%r15), %rsi
	movq	BHW_X_TR(%r15), %rdi
	TSS_AVAILABLE %rsi, %rdi
	movq	BHW_X_TR(%r15), %rax
	ltr	%ax
	/* CS: the selector the foreign world ran with (0x08) */
	pushq	BHW_X_CS(%r15)
	leaq	1f(%rip), %rax
	pushq	%rax
	lretq
1:	movq	BHW_X_SS(%r15), %rax
	movw	%ax, %ss
	movq	BHW_X_SEL(%r15), %rax
	movw	%ax, %ds
	shrq	$16, %rax
	movw	%ax, %es
	shrq	$16, %rax
	movw	%ax, %fs
	shrq	$16, %rax
	movw	%ax, %gs
	WRMSR_FROM MSR_FS_BASE, BHW_X_FS
	WRMSR_FROM MSR_GS_BASE, BHW_X_GS
	WRMSR_FROM MSR_KGS_BASE, BHW_X_KGS
	WRMSR_FROM MSR_LSTAR, BHW_X_LSTAR
	WRMSR_FROM MSR_STAR, BHW_X_STAR
	WRMSR_FROM MSR_CSTAR, BHW_X_CSTAR
	WRMSR_FROM MSR_SFMASK, BHW_X_SFMASK
	WRMSR_FROM MSR_SE_CS, BHW_X_SE_CS
	WRMSR_FROM MSR_SE_ESP, BHW_X_SE_ESP
	WRMSR_FROM MSR_SE_EIP, BHW_X_SE_EIP
	WRMSR_FROM MSR_TSC_AUX, BHW_X_TSC_AUX
	movq	BHW_X_XCR0(%r15), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xorl	%ecx, %ecx
	xsetbv
	clts					/* xrstor must not fault on TS; CR0 is set right after */
	movl	$-1, %eax
	movl	$-1, %edx
	xrstor64 BHW_XSAVE(%r15)
	movq	BHW_X_CR0(%r15), %rax
	movq	%rax, %cr0
	lidt	BHW_X_IDTR(%r15)
	movq	$1, BHW_IN_FOREIGN(%r15)
	ANNOTATE_RETPOLINE_SAFE
	jmp	*%r14
SYM_CODE_END(to_foreign)

/*
 * long bh_world_enter(struct bh_world *w): first entry into the foreign world, at w->start_rip with start_rsp, rdi,
 * rsi, interrupts off. Called with interrupts off, preemption off and the FPU claimed (kernel_fpu_begin). Returns the
 * handler's BH_ABORT result when the run on this CPU ends.
 */
SYM_FUNC_START(bh_world_enter)
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rdi, %r15
	movq	%rsp, BHW_ABORT_RSP(%r15)
	movq	%rsp, BHW_L_RSP(%r15)
	call	to_foreign
	movq	BHW_START_RSP(%r15), %rsp
	movq	BHW_START_RDI(%r15), %rdi
	movq	BHW_START_RSI(%r15), %rsi
	movq	BHW_START_RIP(%r15), %rax
	xorl	%ebp, %ebp
	pushq	$0				/* no return address: the entry point never returns */
	cld
	ANNOTATE_RETPOLINE_SAFE
	jmp	*%rax
SYM_FUNC_END(bh_world_enter)
STACK_FRAME_NON_STANDARD bh_world_enter

/*
 * The host-call entry, called from foreign code: rdi = nr, rsi = a1, rdx = a2; returns the handler's result in rax.
 * Preserves rbx, rbp, r12-r15 and the interrupt flag.
 */
SYM_CODE_START(bh_hostcall_trampoline)
	UNWIND_HINT_UNDEFINED
	ENDBR
	pushfq
	cli
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	rdpid	%rax
	andl	$0xffff, %eax
	shlq	$14, %rax			/* sizeof(struct bh_world) == 16 KiB */
	leaq	bh_worlds(%rip), %r15
	addq	%rax, %r15
	movq	%rdi, BHW_NR(%r15)
	movq	%rsi, BHW_A1(%r15)
	movq	%rdx, BHW_A2(%r15)
	movq	%rsp, BHW_X_RSP(%r15)
	/* the foreign world's state */
	movq	%cr3, %rax
	movq	%rax, BHW_X_CR3(%r15)
	sgdt	BHW_X_GDTR(%r15)
	sidt	BHW_X_IDTR(%r15)
	xorl	%eax, %eax
	str	%ax
	movq	%rax, BHW_X_TR(%r15)
	movl	%cs, %eax
	movq	%rax, BHW_X_CS(%r15)
	movl	%ss, %eax
	movq	%rax, BHW_X_SS(%r15)
	movl	%gs, %eax
	shlq	$16, %rax
	movw	%fs, %ax
	shlq	$16, %rax
	movw	%es, %ax
	shlq	$16, %rax
	movw	%ds, %ax
	movq	%rax, BHW_X_SEL(%r15)
	RDMSR_TO MSR_FS_BASE, BHW_X_FS
	RDMSR_TO MSR_GS_BASE, BHW_X_GS
	RDMSR_TO MSR_KGS_BASE, BHW_X_KGS
	RDMSR_TO MSR_LSTAR, BHW_X_LSTAR
	RDMSR_TO MSR_STAR, BHW_X_STAR
	RDMSR_TO MSR_CSTAR, BHW_X_CSTAR
	RDMSR_TO MSR_SFMASK, BHW_X_SFMASK
	RDMSR_TO MSR_SE_CS, BHW_X_SE_CS
	RDMSR_TO MSR_SE_ESP, BHW_X_SE_ESP
	RDMSR_TO MSR_SE_EIP, BHW_X_SE_EIP
	RDMSR_TO MSR_TSC_AUX, BHW_X_TSC_AUX
	xorl	%ecx, %ecx
	xgetbv
	shlq	$32, %rdx
	orq	%rdx, %rax
	movq	%rax, BHW_X_XCR0(%r15)
	movq	%cr0, %rax
	movq	%rax, BHW_X_CR0(%r15)
	clts
	movl	$-1, %eax
	movl	$-1, %edx
	xsave64	BHW_XSAVE(%r15)
	call	to_linux
	/* the host call, as ordinary C on the Linux thread's stack */
	movq	BHW_L_RSP(%r15), %rsp
	/*
	 * A frame chain Linux can unwind (stack traces taken in a handler, or in an interrupt forwarded from it): rbp
	 * still holds the foreign world's value; point it at bh_world_enter's frame (5 registers above its rbp).
	 */
	leaq	40(%rsp), %rbp
	movq	%r15, %rdi
	call	bh_world_dispatch
	cmpq	$BH_ABORT_VALUE, %rax
	je	.Labort
	movq	%rax, BHW_RESULT(%r15)
	call	to_foreign
	movq	BHW_X_RSP(%r15), %rsp
	movq	BHW_RESULT(%r15), %rax
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	popfq
	ANNOTATE_UNRET_SAFE
	ret
.Labort:
	/* leave the foreign world for good: return from bh_world_enter with the abort result */
	movq	BHW_ABORT_RSP(%r15), %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ANNOTATE_UNRET_SAFE
	ret
SYM_CODE_END(bh_hostcall_trampoline)

/*
 * Re-delivery of hardware interrupts to Linux: bh_int_table + 8 * vector is { endbr64; int $vector; ret; nop }.
 */
	.align	16
SYM_CODE_START(bh_int_table)
	vec = 0
	.rept	256
	.byte	0xf3, 0x0f, 0x1e, 0xfa		/* endbr64 */
	.byte	0xcd, vec			/* int $vec */
	ANNOTATE_UNRET_SAFE
	.byte	0xc3, 0x90			/* ret; nop */
	vec = vec + 1
	.endr
SYM_CODE_END(bh_int_table)

/*
 * Transition IDT handlers: an NMI during a switch is dropped (iretq), anything else is a bug and spins with
 * interrupts off (the soft-lockup detector reports it).
 */
SYM_CODE_START(bh_tidt_nmi)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	iretq
SYM_CODE_END(bh_tidt_nmi)

SYM_CODE_START(bh_tidt_other)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
1:	pause
	jmp	1b
SYM_CODE_END(bh_tidt_other)

/*
 * The initial foreign IDT: every vector reports itself through a host call (BH_HC_TRAP, a1 = vector, a2 = rip) and,
 * if the handler returns, spins. bh_boot_vectors + 16 * vector.
 */
	.align	16
SYM_CODE_START(bh_boot_vectors)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	vec = 0
	.rept	256
	.align	16
	.if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
	pushq	$vec
	.else
	pushq	$0
	pushq	$vec
	.endif
	jmp	bh_boot_trap
	vec = vec + 1
	.endr
SYM_CODE_END(bh_boot_vectors)

SYM_CODE_START_LOCAL(bh_boot_trap)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	movq	(%rsp), %rsi
	movq	16(%rsp), %rdx
	movl	$BH_HC_TRAP, %edi
	andq	$-16, %rsp
	call	bh_hostcall_trampoline
1:	pause
	jmp	1b
SYM_CODE_END(bh_boot_trap)
