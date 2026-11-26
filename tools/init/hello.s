// SPDX-License-Identifier: GPL-2.0-only
// Minimal static x86_64 /sbin/hello: prove execve and wait4 exit status.
	.text
	.equ	SYSCALL_CLASS_UNIX, 0x02000000
	.globl	_start
	.p2align	4
_start:
	andq	$-16, %rsp

	// launchd has already opened the console on fds 0/1/2, so this open
	// normally returns fd 3. Using the returned descriptor also works when
	// hello is exec'd without initialized stdio.
	leaq	Lconsole_path(%rip), %rdi
	movl	$1, %esi			// O_WRONLY
	xorl	%edx, %edx
	movl	$(SYSCALL_CLASS_UNIX | 5), %eax			// SYS_open
	syscall
	jc	Lopen_failed
	movq	%rax, %rdi
	leaq	Lmessage(%rip), %rsi
	movl	$(Lmessage_end - Lmessage), %edx
	movl	$(SYSCALL_CLASS_UNIX | 4), %eax			// SYS_write
	syscall
	jc	Lwrite_failed
	cmpq	$(Lmessage_end - Lmessage), %rax
	jne	Lshort_write

	movl	$42, %edi			// wait4 must report 42 (0x2a00)
Lexit:
	movl	$(SYSCALL_CLASS_UNIX | 1), %eax			// SYS_exit
	syscall
Lhang:						// exit must not return
	jmp	Lhang

Lopen_failed:
	movl	$90, %edi
	jmp	Lexit
Lwrite_failed:
	movl	$91, %edi
	jmp	Lexit
Lshort_write:
	movl	$92, %edi
	jmp	Lexit

	.section	__TEXT,__cstring,cstring_literals
Lconsole_path:
	.asciz	"/dev/console"
Lmessage:
	.ascii	"[hello] userspace exec ok\n"
Lmessage_end:
