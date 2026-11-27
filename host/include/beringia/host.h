/* SPDX-License-Identifier: MIT */
/*
 * beringia_host.ko: the interface a foreign-kernel module (beringia_xnu.ko) uses.
 *
 * Plain integer types and opaque handles only; nothing of Linux's crosses it. Exported with EXPORT_SYMBOL (not _GPL)
 * so that a module under another license can link to it, as with any stable driver interface.
 *
 * Model (docs/architecture.md): a foreign CPU is a Linux kernel thread bound to one Linux CPU. It enters the foreign
 * world (bh_cpu_start) and stays there; it comes back to the Linux world only for host calls, which the foreign kernel
 * makes by calling bh_hostcall_entry(), and in which the module's handler runs as ordinary kernel-thread code.
 */
#ifndef BERINGIA_HOST_H
#define BERINGIA_HOST_H

#define BH_VERSION      1
#define BH_MAX_CPUS     64

#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 bh_u64;
#else
typedef unsigned long long bh_u64;
#endif

/* Handler return value that ends the run on this CPU (the thread leaves the foreign world for good). */
#define BH_ABORT        (-4096L)

/* ---- memory ---- */

struct bh_mem_range {
	bh_u64 pa;
	bh_u64 size;
};

/* The carve-outs (RAM Linux does not manage) claimed at load; returns how many, fills at most max. */
int bh_mem_ranges(struct bh_mem_range *r, int max);
/* The host's address of [pa, pa + len) when it lies within one carve-out, else NULL. */
void *bh_phys(bh_u64 pa, bh_u64 len);

/* ---- the foreign world ---- */

/* Initial state of a foreign CPU. Interrupts are off; the host provides a temporary GDT (0x08 64-bit code, 0x10 data,
 * 0x40 TSS: the XNU layout) and an IDT that ends the run on any exception, until the foreign kernel loads its own. */
struct bh_world_start {
	bh_u64 cr3;             /* must map the module area (Linux PML4[511] PDPT entries 510/511) */
	bh_u64 rip;
	bh_u64 rsp;
	bh_u64 rdi;
	bh_u64 rsi;
};

/* A host call: nr and two arguments as the foreign kernel passed them; the result goes back in rax. Runs in the Linux
 * world on the CPU thread, interrupts off; it may enable them, sleep, and use any kernel service. */
typedef long (*bh_hostcall_fn)(int cpu, unsigned long nr, unsigned long a1, unsigned long a2);

/* The address foreign code calls for host calls (SysV: rdi = nr, rsi = a1, rdx = a2). Mapped wherever the module is. */
unsigned long bh_hostcall_entry(void);

/* Start foreign CPU `cpu` (0..BH_MAX_CPUS-1) in a thread bound to Linux CPU `linux_cpu`. */
int bh_cpu_start(int cpu, int linux_cpu, const struct bh_world_start *st, bh_hostcall_fn fn);
/* Ask every running foreign CPU to stop (its next host call returns BH_ABORT) and wait for the threads to exit. */
void bh_cpu_stop_all(void);
/* Nonzero once a stop was requested for this CPU (a handler can return BH_ABORT itself). */
int bh_stop_requested(int cpu);

/* From a handler: re-deliver a hardware interrupt vector to Linux (runs Linux's handler, which sends the EOI). */
void bh_reinject_irq(unsigned int vector);
/* From a handler: let Linux schedule other work on this CPU if it wants to (RCU, fairness). */
void bh_yield(void);
/* From a handler: wait until the TSC passes deadline (0: no deadline), *wake becomes nonzero, or a kick. */
void bh_idle(int cpu, bh_u64 tsc_deadline, const volatile unsigned int *wake);
/* Wake a CPU waiting in bh_idle (from any context). */
void bh_kick(int cpu);
/* Arm (or with 0 cancel) this CPU's one-shot timer; fire(cpu) runs in hard-interrupt context when it expires. */
void bh_timer_set(int cpu, bh_u64 tsc_deadline, void (*fire)(int cpu));

/* ---- facts about the host ---- */

bh_u64 bh_tsc_hz(void);
/* Linux's PML4[511] PDPT (all 512 entries: its kernel image, modules and fixmap are at 510/511), for the foreign
 * kernel's own tables and to keep the foreign kernel's virtual space below Linux's part of that slot. */
void bh_linux_pdpt(bh_u64 out[512]);
/* used[i] = 1 for each PML4 slot (256..511) Linux has a mapping in. */
void bh_pml4_used(unsigned char used[512]);
/* Bring-up log: one line to the kernel log and to the trace file (beringia_host.ko trace=PATH). */
void bh_log(const char *s);
void bh_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Console output from a host call: to dmesg and, synchronously, to the trace file (survives a VM crash). */
void bh_console(const char *s);
/* A host file for the foreign kernel's devices (disk images): open, size, positional read/write. */
void *bh_file_open(const char *path, int writable, bh_u64 *size);
long bh_file_rw(void *f, int write, void *buf, unsigned long len, bh_u64 off);
/* Everything written so far on stable storage (fsync); 0 or a negative errno. May sleep. */
long bh_file_sync(void *f);
void bh_file_close(void *f);
/* Random bytes from the host's CSPRNG; wall clock (UTC). */
void bh_random(void *buf, unsigned long len);
void bh_wallclock(bh_u64 *sec, bh_u64 *nsec);

#endif /* BERINGIA_HOST_H */
