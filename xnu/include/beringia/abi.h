/* SPDX-License-Identifier: MIT */
/*
 * Beringia hosted ABI: the contract between a hosted x86_64 XNU and its host (beringia_xnu.ko).
 *
 * Shared by both sides; includes nothing. Copied into XNU's tree as EXTERNAL_HEADERS/beringia/abi.h by
 * scripts/xnu/prepare.sh, included by loader/ from here.
 *
 * Calls. XNU calls the host through the function pointer `beringia_hook` (set by the loader before XNU runs) with the
 * SysV convention: rdi = call number (enum beringia_call), rsi = a1, rdx = a2; rax = result. Caller-saved registers are
 * clobbered, callee-saved ones preserved. A call may be made with interrupts enabled or disabled, from any XNU context;
 * the host switches to the Linux world for its duration (docs/architecture.md, section 3).
 *
 * Memory. The host never dereferences XNU virtual addresses except inside the kernel image, whose physical address is
 * its virtual address minus BERINGIA_KERNEL_BASE (the loader keeps that invariant). Everything else crosses the ABI as
 * physical addresses (like a DMA device's buffers).
 *
 * Shared state. `struct beringia_shared` lives in XNU's __DATA as the symbol `beringia_shared`; the loader finds it in
 * the symbol table, fills the boot-time fields and the host updates the per-CPU pending words.
 */
#ifndef BERINGIA_ABI_H
#define BERINGIA_ABI_H

#define BERINGIA_ABI_VERSION    3       /* 1: block calls 34/35; 2: beringia_shared.dump_filter; 3: kernel_abi */
#define BERINGIA_KERNEL_BASE    0xffffff8000000000ULL   /* x86_64 XNU KERNEL_BASE: image PA = VA - this */
#define BERINGIA_MAX_CPUS       64

/* Standalone kexts cannot import the kernel's non-KPI beringia_call symbol. The loader therefore publishes the same
 * entry address it writes to beringia_hook as this numeric boot argument; a hosted kext may use the calling convention
 * above. It is a kernel virtual address in the shared host-module mapping, not a physical address. */
#define BERINGIA_HOSTCALL_BOOT_ARG  "beringia_hc"

enum beringia_call {
	/* console and diagnostics */
	BERINGIA_CALL_CONSOLE       = 1,    /* a1 = cpu: shared.cpu[a1].console holds a2 bytes of console output */
	BERINGIA_CALL_PANIC         = 2,    /* shared.cpu[a1].console holds the panic message; the run ends */
	BERINGIA_CALL_HALT          = 3,    /* a1 = reason (0 = halt, 1 = reboot): the run ends */
	BERINGIA_CALL_MARK          = 4,    /* a1 = bring-up checkpoint number, a2 = value: logged by the host */

	/* time, interrupts, CPUs */
	BERINGIA_CALL_FORWARD_IRQ   = 16,   /* a1 = vector: re-deliver this hardware vector to Linux (from interrupt()) */
	BERINGIA_CALL_SET_TIMER     = 17,   /* a1 = TSC deadline of this CPU's next XNU timer (0 = none) */
	BERINGIA_CALL_IDLE          = 18,   /* a1 = TSC deadline: run Linux until then, a kick, or a forwarded interrupt */
	BERINGIA_CALL_IPI           = 19,   /* a1 = target cpu: its pending word has bits set; make it look */
	BERINGIA_CALL_CPU_START     = 20,   /* a1 = cpu, a2 = PA of struct beringia_cpu_start: start that CPU's thread */
	BERINGIA_CALL_YIELD         = 21,   /* a busy CPU offers Linux the chance to run (spin loops, long kernel paths) */

	/* services */
	BERINGIA_CALL_WALLCLOCK     = 32,   /* a1 = PA of u64[2] {seconds, nanoseconds} UTC */
	BERINGIA_CALL_RANDOM        = 33,   /* a1 = PA, a2 = length (<= 4096, within one page): host CSPRNG bytes */
	BERINGIA_CALL_BLOCK_RW      = 34,   /* a1 = PA of struct beringia_block_req, a2 = 0; result is in the request */
	BERINGIA_CALL_BLOCK_INFO    = 35,   /* a1 = PA of struct beringia_block_info, a2 = 0; host fills the structure */
};

/* Bits of the per-CPU pending word (set by the host or another CPU, taken by XNU in its interrupt context). */
#define BERINGIA_PENDING_TIMER  (1u << 0)   /* the CPU's timer deadline passed */
#define BERINGIA_PENDING_IPI    (1u << 1)   /* another XNU CPU signalled this one */
#define BERINGIA_PENDING_STOP   (1u << 2)   /* the host is ending the run */
#define BERINGIA_PENDING_DUMP   (1u << 3)   /* bring-up: print every thread's state and backtrace */

struct beringia_cpu_shared {
	volatile unsigned int pending;      /* BERINGIA_PENDING_* */
	unsigned int apic_id;               /* host-assigned, what XNU reports as this CPU's APIC id */
	unsigned long long timer_deadline;  /* TSC; mirrors the last BERINGIA_CALL_SET_TIMER */
	char console[2048];                 /* console / panic text for CONSOLE and PANIC */
};

struct beringia_shared {
	/* filled by the loader before XNU runs */
	unsigned int abi_version;           /* BERINGIA_ABI_VERSION */
	unsigned int ncpus;
	unsigned long long tsc_hz;          /* the host's TSC frequency */
	unsigned long long linux_pdpt_510;  /* Linux's PML4[511] PDPT entries 510 and 511 (module/kernel text, fixmap) */
	unsigned long long linux_pdpt_511;
	unsigned int physmap_pml4;          /* PML4 slot for XNU's physmap (free in Linux's tables) */
	unsigned int dblmap_pml4;           /* PML4 slot for XNU's double map */
	unsigned long long kva_ceiling;     /* XNU must not map kernel VAs at or above this (Linux's region in slot 511) */
	unsigned long long phys_top;        /* highest physical address (exclusive) of XNU's RAM: the physmap covers it */
	/* per CPU */
	struct beringia_cpu_shared cpu[BERINGIA_MAX_CPUS];
	/* bring-up, set by the host before BERINGIA_PENDING_DUMP: the thread dump also prints the user-space state and
	 * frame-pointer backtrace of threads whose process name contains this string ("*": every process; "" or "1":
	 * kernel stacks only); "peek:PID:ADDR:LEN" prints LEN (<= 4096) bytes of that process's memory instead.
	 * NUL-terminated. Appended last: the offsets above do not change. */
	char dump_filter[32];
	/* written by XNU (BERINGIA_ABI_VERSION of the header it was built with) the first time it calls the host, so the
	 * loader can tell a kernel without dump_filter (0) from its own; it is 0 for a kernel built before this field. */
	unsigned int kernel_abi;
};

/* BERINGIA_CALL_CPU_START: where a secondary CPU enters XNU. */
struct beringia_cpu_start {
	unsigned long long entry;           /* XNU VA to jump to, in the XNU world, interrupts off */
	unsigned long long stack;           /* XNU VA of its initial stack top */
	unsigned long long arg;             /* passed in rdi */
	unsigned long long cr3;             /* XNU's kernel CR3 */
};

/* BERINGIA_CALL_BLOCK_RW: one request on host disk unit 0 (loader disk=PATH), data in physically contiguous segments.
 * The host validates every segment with bh_phys before starting an operation. For read/write, offset is the file byte
 * offset and segments are contiguous in device order. result is the total bytes moved, or a negative Linux errno.
 * Flush currently completes outstanding host-file writes but does not promise an fsync to stable storage. */
#define BERINGIA_BLOCK_SIZE       512
#define BERINGIA_BLOCK_MAX_SEGS   16
#define BERINGIA_BLOCK_READ       0
#define BERINGIA_BLOCK_WRITE      1
#define BERINGIA_BLOCK_FLUSH      2

struct beringia_block_seg {
	unsigned long long pa;
	unsigned long long len;
};

struct beringia_block_req {
	unsigned int unit;
	unsigned int write;                 /* BERINGIA_BLOCK_READ, _WRITE or _FLUSH */
	unsigned long long offset;          /* bytes; ignored for flush */
	unsigned int nseg;                  /* 0..BERINGIA_BLOCK_MAX_SEGS; must be 0 for flush */
	unsigned int pad;
	long long result;                   /* bytes transferred or -errno; 0 for a successful flush */
	struct beringia_block_seg seg[BERINGIA_BLOCK_MAX_SEGS];
};

struct beringia_block_info {
	unsigned int unit;
	unsigned int block_size;
	unsigned long long size;            /* bytes; 0 when the unit does not exist */
	unsigned int read_only;
	unsigned int pad;
};

#endif /* BERINGIA_ABI_H */
