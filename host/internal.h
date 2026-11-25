/* SPDX-License-Identifier: GPL-2.0-only */
/* beringia_host.ko internals shared between its files. */
#ifndef BERINGIA_HOST_INTERNAL_H
#define BERINGIA_HOST_INTERNAL_H

#include <beringia/host.h>

/* mem.c */
int bh_mem_init(const char *carve);
void bh_mem_exit(void);

/* cpu.c */
int bh_cpu_init(void);
void bh_cpu_exit(void);


/* selftest.c */
int bh_selftest(unsigned int seconds, unsigned int ncpus, unsigned int spin);

#endif
