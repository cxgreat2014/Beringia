/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The self-test's foreign kernel: a few hundred bytes of code that run in the foreign world the way XNU will, with
 * their own CR3 (a copy of Linux's top-level table), GDT (XNU's selector layout), IDT and TSS (IST1 for interrupts).
 *
 *  - bh_st_main: interrupts on, spin, execute ud2 (handled by its own IDT: the #UD handler skips it), optionally
 *    change IA32_PAT behind the host's back (as XNU's pat_init would), make a host call, repeat. Never returns: the
 *    host ends the run from a host call.
 *  - bh_st_vectors: external interrupts, forwarded to the host with a host call (as XNU's interrupt() will).
 *
 * Executed in the foreign world: SYM_CODE, plain ret, excluded from objtool (see world.S).
 */
#include <linux/linkage.h>
#include <linux/objtool.h>
#include <asm/unwind_hints.h>

#define HC_FORWARD_IRQ  16
#define HC_YIELD        21

	.text

SYM_CODE_START(bh_st_main)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	movq	%rdi, %rbx			/* spin count between host calls */
	movq	%rsi, %r12			/* flags: bit 0 = tamper with IA32_PAT (the host must notice and restore it) */
1:	sti
	movq	%rbx, %rcx
2:	pause
	decq	%rcx
	jnz	2b
	ud2
	lock incq bh_st_loops(%rip)
	testq	$1, %r12
	jz	3f
	movl	$0x277, %ecx			/* IA32_PAT: flip bit 0 of PA6 (UC- <-> WB, WC <-> UC; always a valid type) */
	rdmsr
	xorl	$0x10000, %edx
	wrmsr
3:
	movl	$HC_YIELD, %edi
	xorl	%esi, %esi
	xorl	%edx, %edx
	call	bh_hostcall_trampoline
	jmp	1b
SYM_CODE_END(bh_st_main)

/* #UD: skip the 2-byte ud2 */
SYM_CODE_START(bh_st_ud)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	addq	$2, (%rsp)
	lock incq bh_st_uds(%rip)
	iretq
SYM_CODE_END(bh_st_ud)

/* vectors 32..255: bh_st_vectors + 16 * (vector - 32) */
	.align	16
SYM_CODE_START(bh_st_vectors)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	vec = 32
	.rept	224
	.align	16
	pushq	$vec
	jmp	bh_st_irq
	vec = vec + 1
	.endr
SYM_CODE_END(bh_st_vectors)

SYM_CODE_START_LOCAL(bh_st_irq)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	movq	72(%rsp), %rsi			/* the vector */
	movl	$HC_FORWARD_IRQ, %edi
	xorl	%edx, %edx
	call	bh_hostcall_trampoline
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax
	addq	$8, %rsp
	iretq
SYM_CODE_END(bh_st_irq)

	.data
	.align	8
SYM_DATA(bh_st_loops, .quad 0)
SYM_DATA(bh_st_uds, .quad 0)
