/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * x86_64 assembly portion of fpcheck, adapted from the AArch64 loop in
 * xnulinux/tests/host/fpcheck.c (GPL-2.0-only).
 *
 * No compiler-generated code runs in this loop.  Raw syscalls are used for
 * clock_gettime, sched_yield, and nanosleep because the x86_64 System V ABI
 * allows every XMM register to be clobbered by an ordinary function call.
 */

.equ FP_ROUNDS,          0
.equ FP_BAD_ROUNDS,      8
.equ FP_BAD_REGISTERS,  16
.equ FP_CHANGES,        24
.equ FP_FIRST_GOT,     152

.equ SAVED_XMM,          0
.equ SLEEP_TS,         256
.equ NOW_TS,           272
.equ STACK_BYTES,      384

.equ NR_SCHED_YIELD,    24
.equ NR_NANOSLEEP,      35
.equ NR_CLOCK_GETTIME, 228
.equ CLOCK_MONOTONIC,    1

.text
.globl fpcheck_run
.type fpcheck_run, @function
/*
 * int fpcheck_run(uint64_t seconds, int selftest, struct fp_results *out)
 *
 * out is zeroed by the C caller.  Each register is checked independently;
 * both halves of a register changed in one round count as one change.
 */
fpcheck_run:
	pushq %rbp
	movq %rsp, %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq $STACK_BYTES, %rsp

	movq %rdx, %r12		/* results */
	movl %esi, %r15d	/* nonzero requests one deliberate clobber */
	movq %rdi, %r13		/* seconds, then deadline.tv_sec */

	movl $CLOCK_MONOTONIC, %edi
	leaq NOW_TS(%rsp), %rsi
	movl $NR_CLOCK_GETTIME, %eax
	syscall
	testq %rax, %rax
	js .Lerror
	addq NOW_TS(%rsp), %r13
	movq NOW_TS+8(%rsp), %r14	/* deadline.tv_nsec */

.Lround:
	/* Restore all live registers from read-only, aligned known values. */
	movaps fpcheck_patterns+0(%rip), %xmm0
	movaps fpcheck_patterns+16(%rip), %xmm1
	movaps fpcheck_patterns+32(%rip), %xmm2
	movaps fpcheck_patterns+48(%rip), %xmm3
	movaps fpcheck_patterns+64(%rip), %xmm4
	movaps fpcheck_patterns+80(%rip), %xmm5
	movaps fpcheck_patterns+96(%rip), %xmm6
	movaps fpcheck_patterns+112(%rip), %xmm7
	movaps fpcheck_patterns+128(%rip), %xmm8
	movaps fpcheck_patterns+144(%rip), %xmm9
	movaps fpcheck_patterns+160(%rip), %xmm10
	movaps fpcheck_patterns+176(%rip), %xmm11
	movaps fpcheck_patterns+192(%rip), %xmm12
	movaps fpcheck_patterns+208(%rip), %xmm13
	movaps fpcheck_patterns+224(%rip), %xmm14
	movaps fpcheck_patterns+240(%rip), %xmm15

	testl %r15d, %r15d
	jz .Lno_selftest
	xorps %xmm7, %xmm7	/* deliberate one-time change to all zeroes */
	xorl %r15d, %r15d
.Lno_selftest:

	movl $20, %ebx
.Lyield:
	movl $NR_SCHED_YIELD, %eax
	syscall
	decl %ebx
	jnz .Lyield

	movq $0, SLEEP_TS(%rsp)
	movq $200000, SLEEP_TS+8(%rsp)	/* 200 microseconds */
	leaq SLEEP_TS(%rsp), %rdi
	xorl %esi, %esi
	movl $NR_NANOSLEEP, %eax
	syscall				/* EINTR is fine: verify either way */

	/* Save every live register before using any SSE instruction again. */
	movdqu %xmm0, SAVED_XMM+0(%rsp)
	movdqu %xmm1, SAVED_XMM+16(%rsp)
	movdqu %xmm2, SAVED_XMM+32(%rsp)
	movdqu %xmm3, SAVED_XMM+48(%rsp)
	movdqu %xmm4, SAVED_XMM+64(%rsp)
	movdqu %xmm5, SAVED_XMM+80(%rsp)
	movdqu %xmm6, SAVED_XMM+96(%rsp)
	movdqu %xmm7, SAVED_XMM+112(%rsp)
	movdqu %xmm8, SAVED_XMM+128(%rsp)
	movdqu %xmm9, SAVED_XMM+144(%rsp)
	movdqu %xmm10, SAVED_XMM+160(%rsp)
	movdqu %xmm11, SAVED_XMM+176(%rsp)
	movdqu %xmm12, SAVED_XMM+192(%rsp)
	movdqu %xmm13, SAVED_XMM+208(%rsp)
	movdqu %xmm14, SAVED_XMM+224(%rsp)
	movdqu %xmm15, SAVED_XMM+240(%rsp)

	/* Integer-only comparison: this loop never changes a live XMM value. */
	leaq fpcheck_patterns(%rip), %r8
	xorl %ebx, %ebx		/* register index */
	xorl %r10d, %r10d	/* changed registers in this round */
.Lcompare_register:
	movq %rbx, %rax
	shlq $4, %rax		/* byte offset for a 128-bit register */
	movq SAVED_XMM(%rsp,%rax), %rdx
	movq SAVED_XMM+8(%rsp,%rax), %rcx
	cmpq 0(%r8,%rax), %rdx
	jne .Lchanged
	cmpq 8(%r8,%rax), %rcx
	je .Lnext_register
.Lchanged:
	cmpq $0, FP_CHANGES(%r12,%rbx,8)
	jne .Lhave_first_value
	movq %rdx, FP_FIRST_GOT(%r12,%rax)
	movq %rcx, FP_FIRST_GOT+8(%r12,%rax)
.Lhave_first_value:
	incq FP_CHANGES(%r12,%rbx,8)
	incq FP_BAD_REGISTERS(%r12)
	incq %r10
.Lnext_register:
	incq %rbx
	cmpq $16, %rbx
	jne .Lcompare_register

	testq %r10, %r10
	jz .Lround_was_clean
	incq FP_BAD_ROUNDS(%r12)
.Lround_was_clean:
	incq FP_ROUNDS(%r12)

	movl $CLOCK_MONOTONIC, %edi
	leaq NOW_TS(%rsp), %rsi
	movl $NR_CLOCK_GETTIME, %eax
	syscall
	testq %rax, %rax
	js .Lerror
	movq NOW_TS(%rsp), %rax
	cmpq %r13, %rax
	jg .Ldone
	jl .Lround
	movq NOW_TS+8(%rsp), %rax
	cmpq %r14, %rax
	jl .Lround

.Ldone:
	xorl %eax, %eax
	jmp .Lreturn
.Lerror:
	movl $-1, %eax
.Lreturn:
	addq $STACK_BYTES, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	ret
.size fpcheck_run, .-fpcheck_run

.section .rodata
.p2align 4
.globl fpcheck_patterns
.type fpcheck_patterns, @object
fpcheck_patterns:
.set pattern_index, 0
.rept 16
	.quad 0x0123456789abcdef + pattern_index * 0x1000000000000000
	.quad 0xfedcba9876543210 - pattern_index * 0x1000000000000000
.set pattern_index, pattern_index + 1
.endr
.size fpcheck_patterns, .-fpcheck_patterns

.section .note.GNU-stack,"",@progbits
