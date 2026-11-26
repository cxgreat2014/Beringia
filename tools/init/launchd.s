// SPDX-License-Identifier: GPL-2.0-only
// Minimal static x86_64 /sbin/launchd for the Beringia RAM-disk root.
//
// This uses XNU's raw 64-bit BSD syscall ABI directly: rax contains
// SYSCALL_CONSTRUCT_UNIX(number) (0x02000000 | number), arguments are
// rdi/rsi/rdx/r10/r8/r9, and carry set means that rax contains errno. There is
// no libc, dyld, or commpage dependency.
//
// PID 1 must never exit. Every launchd failure path prints a marker when the
// console is usable and then parks in sigsuspend(0), the raw pause equivalent.

	.text
	.equ	SYSCALL_CLASS_UNIX, 0x02000000
	.globl	_start
	.p2align	4

.macro	PRINT	label
	movl	$1, %edi
	leaq	\label(%rip), %rsi
	movl	$(\label\()_end - \label), %edx
	call	Lwrite
.endm

_start:
	// XNU enters with rsp pointing at argc. Keep a private, aligned scratch
	// area for execve vectors and the wait4 status.
	andq	$-16, %rsp
	subq	$64, %rsp

	// The initial process has no useful stdio descriptors. Three opens make
	// /dev/console fds 0, 1, and 2, which /sbin/hello inherits across execve.
	movl	$3, %r12d
Lopen_console:
	leaq	Lconsole_path(%rip), %rdi
	movl	$1, %esi			// O_WRONLY
	xorl	%edx, %edx
	movl	$(SYSCALL_CLASS_UNIX | 5), %eax			// SYS_open
	syscall
	jc	Lpark				// no console yet, but never exit PID 1
	subl	$1, %r12d
	jnz	Lopen_console

	PRINT	Lbanner

	// Print before every tested operation so the last line identifies a hang.
	PRINT	Lgetpid
	movl	$(SYSCALL_CLASS_UNIX | 20), %eax			// SYS_getpid
	syscall
	jc	Lgetpid_failed
	testl	%eax, %eax
	jle	Lgetpid_failed
	PRINT	Lgetpid_ok

	PRINT	Lfork
	movl	$(SYSCALL_CLASS_UNIX | 2), %eax			// SYS_fork
	syscall
	jc	Lfork_failed
	// Unlike most syscalls, fork returns a second value in rdx: zero in the
	// parent and one in the child (see x86_64 thread_set_child in XNU).
	testq	%rdx, %rdx
	jnz	Lchild
	movq	%rax, %r12			// child pid

	PRINT	Lwait4
	movq	%r12, %rdi
	leaq	32(%rsp), %rsi			// int status
	xorl	%edx, %edx			// options = 0
	xorl	%r10d, %r10d			// rusage = NULL
	movl	$(SYSCALL_CLASS_UNIX | 7), %eax			// SYS_wait4
	syscall
	jc	Lwait4_failed
	cmpq	%r12, %rax
	jne	Lwait4_failed

	// Render the actual 32-bit wait status before validating it. For the
	// expected exit(42), this prints 0x00002a00.
	movl	32(%rsp), %r14d
	movl	%r14d, %eax
	leaq	Lstatus_digits + 7(%rip), %rdi
	movl	$8, %ecx
Lhex_digit:
	movl	%eax, %edx
	andl	$15, %edx
	cmpl	$10, %edx
	jb	Lhex_decimal
	addl	$('a' - '0' - 10), %edx
Lhex_decimal:
	addl	$'0', %edx
	movb	%dl, (%rdi)
	decq	%rdi
	shrl	$4, %eax
	decl	%ecx
	jnz	Lhex_digit
	PRINT	Lstatus_msg

	// WIFEXITED(status) and WEXITSTATUS(status) == 42.
	movl	%r14d, %eax
	andl	$0x7f, %eax
	jnz	Lbad_status
	movl	%r14d, %eax
	shrl	$8, %eax
	andl	$0xff, %eax
	cmpl	$42, %eax
	jne	Lbad_status
	PRINT	Lchild_exit_42
	PRINT	Linit_ok
	PRINT	Lpause

Lpark:
	// Darwin libc implements pause using sigsuspend. The BSD syscall takes
	// the signal mask by value, not by pointer. It returns EINTR on a signal;
	// loop forever rather than allowing an exiting-init kernel panic.
	movl	$(SYSCALL_CLASS_UNIX | 111), %eax			// SYS_sigsuspend
	xorl	%edi, %edi
	syscall
	jmp	Lpark

Lchild:
	PRINT	Lexecve
	leaq	Lhello_path(%rip), %rdi
	movq	%rdi, 0(%rsp)			// argv[0] = "/sbin/hello"
	movq	$0, 8(%rsp)			// argv[1] = NULL
	movq	$0, 16(%rsp)			// envp[0] = NULL
	leaq	0(%rsp), %rsi
	leaq	16(%rsp), %rdx
	movl	$(SYSCALL_CLASS_UNIX | 59), %eax			// SYS_execve
	syscall
	// A successful execve does not return. Only the child exits here; the
	// parent remains alive to report the unexpected status and park.
	PRINT	Lexecve_failed
	movl	$111, %edi
	movl	$(SYSCALL_CLASS_UNIX | 1), %eax			// SYS_exit
	syscall
	jmp	Lpark

Lgetpid_failed:
	PRINT	Lgetpid_fail_msg
	jmp	Lpark
Lfork_failed:
	PRINT	Lfork_fail_msg
	jmp	Lpark
Lwait4_failed:
	PRINT	Lwait4_fail_msg
	jmp	Lpark
Lbad_status:
	PRINT	Lbad_status_msg
	jmp	Lpark

// write(rdi, rsi, rdx), checking both errno and short writes. If console
// output itself fails there is nowhere left to report the error, so park.
Lwrite:
	pushq	%rdx
	movl	$(SYSCALL_CLASS_UNIX | 4), %eax			// SYS_write
	syscall
	popq	%rdx
	jc	Lpark
	cmpq	%rdx, %rax
	jne	Lpark
	ret

	.section	__TEXT,__cstring,cstring_literals
Lconsole_path:
	.asciz	"/dev/console"
Lhello_path:
	.asciz	"/sbin/hello"
Lbanner:
	.ascii	"[init] x86_64 static launchd\n"
Lbanner_end:
Lgetpid:
	.ascii	"[init] getpid\n"
Lgetpid_end:
Lgetpid_ok:
	.ascii	"[init] getpid ok\n"
Lgetpid_ok_end:
Lgetpid_fail_msg:
	.ascii	"[init] getpid failed\n"
Lgetpid_fail_msg_end:
Lfork:
	.ascii	"[init] fork\n"
Lfork_end:
Lfork_fail_msg:
	.ascii	"[init] fork failed\n"
Lfork_fail_msg_end:
Lexecve:
	.ascii	"[init] execve /sbin/hello\n"
Lexecve_end:
Lexecve_failed:
	.ascii	"[init] execve failed\n"
Lexecve_failed_end:
Lwait4:
	.ascii	"[init] wait4\n"
Lwait4_end:
Lwait4_fail_msg:
	.ascii	"[init] wait4 failed\n"
Lwait4_fail_msg_end:
Lchild_exit_42:
	.ascii	"[init] child exit 42\n"
Lchild_exit_42_end:
Lbad_status_msg:
	.ascii	"[init] unexpected child status\n"
Lbad_status_msg_end:
Linit_ok:
	.ascii	"[init] INIT OK\n"
Linit_ok_end:
Lpause:
	.ascii	"[init] pause forever\n"
Lpause_end:

	.section	__DATA,__data
	.p2align	3
Lstatus_msg:
	.ascii	"[init] wait4 status 0x"
Lstatus_digits:
	.ascii	"00000000"
	.ascii	"\n"
Lstatus_msg_end:
