// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adapted from xnulinux/tests/host/fpcheck.c (GPL-2.0-only) for
 * x86_64 Linux userspace.
 *
 * Pin this thread to one CPU, keep known values in xmm0..xmm15 across
 * context switches, and count changes made behind Linux's back.
 *
 * The complete timing/yield/sleep/verification loop is in fpcheck_asm.S,
 * not generated C.  That is important on x86_64: every XMM register is
 * caller-saved in the System V ABI, so calling libc from the live-register
 * loop would make even an otherwise correct libc clobber look like XNU.
 * The assembly loop uses raw Linux syscalls instead.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct fp_results {
	uint64_t rounds;
	uint64_t bad_rounds;
	uint64_t bad_registers;
	uint64_t changes[16];
	uint64_t first_got[16][2];
};

extern const uint64_t fpcheck_patterns[16][2];
extern int fpcheck_run(uint64_t seconds, int selftest,
		       struct fp_results *results);

_Static_assert(offsetof(struct fp_results, bad_rounds) == 8,
	       "assembly results layout");
_Static_assert(offsetof(struct fp_results, bad_registers) == 16,
	       "assembly results layout");
_Static_assert(offsetof(struct fp_results, changes) == 24,
	       "assembly results layout");
_Static_assert(offsetof(struct fp_results, first_got) == 152,
	       "assembly results layout");
_Static_assert(sizeof(struct fp_results) == 408,
	       "assembly results layout");

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text[0] || text[0] == '-')
		return -1;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end)
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--cpu N] [--seconds N] [--selftest]\n"
		"       %s [CPU [SECONDS]]\n"
		"Defaults: CPU 0, 60 seconds.  --selftest clears xmm7 once.\n",
		program, program);
}

int main(int argc, char **argv)
{
	uint64_t cpu_number = 0, seconds = 60;
	uint64_t i;
	struct fp_results results = { 0 };
	cpu_set_t affinity;
	int selftest = 0;
	int option;
	static const struct option options[] = {
		{ "cpu", required_argument, NULL, 'c' },
		{ "seconds", required_argument, NULL, 't' },
		{ "selftest", no_argument, NULL, 1000 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((option = getopt_long(argc, argv, "c:t:h", options, NULL)) != -1) {
		switch (option) {
		case 'c':
			if (parse_u64(optarg, &cpu_number))
				goto bad_arguments;
			break;
		case 't':
			if (parse_u64(optarg, &seconds))
				goto bad_arguments;
			break;
		case 1000:
			selftest = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			goto bad_arguments;
		}
	}
	if (optind < argc && parse_u64(argv[optind++], &cpu_number))
		goto bad_arguments;
	if (optind < argc && parse_u64(argv[optind++], &seconds))
		goto bad_arguments;
	if (optind != argc || !seconds || cpu_number >= CPU_SETSIZE)
		goto bad_arguments;

	CPU_ZERO(&affinity);
	CPU_SET((int)cpu_number, &affinity);
	if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
		perror("fpcheck: sched_setaffinity");
		return 1;
	}
	printf("fpcheck: pinned to CPU %" PRIu64 ", checking xmm0..xmm15 "
	       "for %" PRIu64 " seconds%s\n", cpu_number, seconds,
	       selftest ? " (selftest)" : "");
	if (fpcheck_run(seconds, selftest, &results) != 0) {
		fprintf(stderr, "fpcheck: clock_gettime syscall failed\n");
		return 1;
	}

	printf("fpcheck: CPU %" PRIu64 ", %" PRIu64 " rounds, %" PRIu64
	       " rounds with changes, %" PRIu64 " XMM register changes\n",
	       cpu_number, results.rounds, results.bad_rounds,
	       results.bad_registers);
	for (i = 0; i < 16; i++) {
		if (!results.changes[i])
			continue;
		printf("xmm%-2" PRIu64 ": changes=%" PRIu64
		       " first_got=0x%016" PRIx64 "%016" PRIx64
		       " expected=0x%016" PRIx64 "%016" PRIx64 "\n",
		       i, results.changes[i], results.first_got[i][1],
		       results.first_got[i][0], fpcheck_patterns[i][1],
		       fpcheck_patterns[i][0]);
	}
	if (selftest) {
		if (results.changes[7]) {
			printf("FPCHECK SELFTEST: PASS (intentional xmm7 clobber detected)\n");
			return 0;
		}
		printf("FPCHECK SELFTEST: FAIL (intentional xmm7 clobber missed)\n");
		return 1;
	}
	printf("FPCHECK: %s\n", results.bad_registers ? "FAIL" : "PASS");
	return results.bad_registers != 0;

bad_arguments:
	usage(argv[0]);
	return 2;
}
