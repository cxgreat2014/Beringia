/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-CPU world state for the x86_64 world switch (world.S), shared between C and assembly.
 *
 * Everything the switch touches lives in the module's own .bss: module memory is mapped in the foreign kernel's page
 * tables too (Linux's PML4[511] PDPT entries 510/511 are copied into them), kmalloc/vmalloc memory is not.
 *
 * Offsets are fixed by hand and checked with static assertions; world.S uses the BHW_* constants.
 */
#ifndef BERINGIA_WORLD_H
#define BERINGIA_WORLD_H

#define BHW_SELF            0
#define BHW_INDEX           8
#define BHW_L_RSP           16
#define BHW_L_CR3           24
#define BHW_L_GDTR          32      /* 16 bytes: limit, base */
#define BHW_L_IDTR          48
#define BHW_L_TR            64
#define BHW_L_GS            72
#define BHW_L_KGS           80
#define BHW_L_FS            88
#define BHW_L_LSTAR         96
#define BHW_L_STAR          104
#define BHW_L_CSTAR         112
#define BHW_L_SFMASK        120
#define BHW_L_TSC_AUX       128
#define BHW_L_SE_CS         136
#define BHW_L_SE_ESP        144
#define BHW_L_SE_EIP        152
#define BHW_L_XCR0          160
#define BHW_L_CR0           168
#define BHW_L_SEL           176     /* ds | es << 16 | fs << 32 | gs << 48 */
#define BHW_L_SS            184
#define BHW_L_GDT_RW        192     /* Linux's writable GDT alias (for the TSS busy bit) */
#define BHW_X_RSP           200
#define BHW_X_CR3           208
#define BHW_X_GDTR          216
#define BHW_X_IDTR          232
#define BHW_X_TR            248
#define BHW_X_GS            256
#define BHW_X_KGS           264
#define BHW_X_FS            272
#define BHW_X_LSTAR         280
#define BHW_X_STAR          288
#define BHW_X_CSTAR         296
#define BHW_X_SFMASK        304
#define BHW_X_TSC_AUX       312
#define BHW_X_SE_CS         320
#define BHW_X_SE_ESP        328
#define BHW_X_SE_EIP        336
#define BHW_X_XCR0          344
#define BHW_X_CR0           352
#define BHW_X_SEL           360
#define BHW_X_SS            368
#define BHW_X_CS            376
#define BHW_FN              384     /* bh_hostcall_fn */
#define BHW_ABORT_RSP       392     /* bh_world_run's frame, for leaving the foreign world for good */
#define BHW_TSTACK_TOP      400     /* transition stack (mapped in both worlds) */
#define BHW_RESULT          408
#define BHW_IN_FOREIGN      416
#define BHW_NR              424
#define BHW_A1              432
#define BHW_A2              440
#define BHW_START_RIP       448     /* first entry */
#define BHW_START_RSP       456
#define BHW_START_RDI       464
#define BHW_START_RSI       472
#define BHW_L_CR4           480     /* Linux's CR4 at the last entry; to_linux puts it back unconditionally */
#define BHW_X_CR4           488     /* the foreign world's CR4 at the last exit (for the safety net) */
#define BHW_BOOT_GDT        512     /* 16 descriptors: the initial foreign GDT (XNU layout) */
#define BHW_BOOT_TSS        640     /* 128 bytes: its TSS */
#define BHW_XSAVE           4096    /* 8 KiB, 64-byte aligned: the foreign FPU/XSAVE state */
#define BHW_TSTACK          12288   /* 4 KiB transition stack */
#define BHW_SIZE            16384

/* TSC_AUX in the foreign world: magic | cpu index, read back with rdpid by the trampolines. */
#define BHW_TSC_AUX_MAGIC   0xbe000000
#define BHW_TSC_AUX_MASK    0xffff0000

/* Selectors. The foreign GDT uses XNU's layout. */
#define BHW_F_CS            0x08
#define BHW_F_SS            0x10
#define BHW_F_TSS           0x40
#define BHW_L_KCS           0x10    /* Linux __KERNEL_CS */
#define BHW_L_KDS           0x18    /* Linux __KERNEL_DS */

#define BH_ABORT_VALUE      (-4096)  /* BH_ABORT in host.h */

#ifndef __ASSEMBLER__
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/build_bug.h>

struct bh_world {
	u64 self, index;
	u64 l_rsp, l_cr3, l_gdtr[2], l_idtr[2], l_tr, l_gs, l_kgs, l_fs;
	u64 l_lstar, l_star, l_cstar, l_sfmask, l_tsc_aux, l_se_cs, l_se_esp, l_se_eip, l_xcr0, l_cr0, l_sel, l_ss;
	u64 l_gdt_rw;
	u64 x_rsp, x_cr3, x_gdtr[2], x_idtr[2], x_tr, x_gs, x_kgs, x_fs;
	u64 x_lstar, x_star, x_cstar, x_sfmask, x_tsc_aux, x_se_cs, x_se_esp, x_se_eip, x_xcr0, x_cr0, x_sel, x_ss, x_cs;
	u64 fn, abort_rsp, tstack_top, result, in_foreign, nr, a1, a2;
	u64 start_rip, start_rsp, start_rdi, start_rsi;
	u64 l_cr4, x_cr4;
	u64 pad1[2];
	u64 boot_gdt[16];
	u8 boot_tss[128];
	u8 pad0[BHW_XSAVE - 768];
	u8 xsave[8192] __aligned(64);
	u8 tstack[4096];
} __aligned(4096);

static_assert(offsetof(struct bh_world, l_rsp) == BHW_L_RSP);
static_assert(offsetof(struct bh_world, l_gdt_rw) == BHW_L_GDT_RW);
static_assert(offsetof(struct bh_world, x_rsp) == BHW_X_RSP);
static_assert(offsetof(struct bh_world, x_cs) == BHW_X_CS);
static_assert(offsetof(struct bh_world, fn) == BHW_FN);
static_assert(offsetof(struct bh_world, a2) == BHW_A2);
static_assert(offsetof(struct bh_world, start_rsi) == BHW_START_RSI);
static_assert(offsetof(struct bh_world, l_cr4) == BHW_L_CR4);
static_assert(offsetof(struct bh_world, x_cr4) == BHW_X_CR4);
static_assert(offsetof(struct bh_world, boot_gdt) == BHW_BOOT_GDT);
static_assert(offsetof(struct bh_world, boot_tss) == BHW_BOOT_TSS);
static_assert(offsetof(struct bh_world, xsave) == BHW_XSAVE);
static_assert(offsetof(struct bh_world, tstack) == BHW_TSTACK);
static_assert(sizeof(struct bh_world) == BHW_SIZE);

extern struct bh_world bh_worlds[];

/* world.S */
long bh_world_enter(struct bh_world *w);        /* first entry; returns only when the run ends (abort) */
void bh_hostcall_trampoline(void);              /* the foreign side's host-call entry */
void bh_int_table(void);                        /* 256 x { int $n; ret } stubs, 4 bytes each */
void bh_boot_vectors(void);                     /* 256 x 16-byte stubs: the initial foreign IDT's handlers */
void bh_tidt_nmi(void);
void bh_tidt_other(void);

/* world.c */
void bh_world_capture_linux(struct bh_world *w);       /* before each Linux -> foreign switch */
long bh_world_dispatch(struct bh_world *w);            /* a host call, in the Linux world */
int bh_world_init(void);                               /* IDTs; checks the CPU */
/* Run foreign CPU index from st until the run ends; called on the CPU thread. */
long bh_world_run(int index, u64 cr3, u64 rip, u64 rsp, u64 rdi, u64 rsi, long (*fn)(int, unsigned long, unsigned long, unsigned long));
/* Install a foreign IDT for the first entry (default: the boot IDT); IST1 stack top for the boot TSS. */
void bh_world_set_entry_idt(struct bh_world *w, void *idt, unsigned int limit, u64 ist1);
/* Number of unswitched Linux CPU state changes the safety net found (and repaired) after foreign-world exits. */
long bh_world_state_diffs(void);
#endif

#endif /* BERINGIA_WORLD_H */
