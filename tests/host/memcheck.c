// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adapted from xnulinux/tests/host/memcheck.c (GPL-2.0-only) for
 * x86_64 Linux userspace.
 *
 * Fill a large mapping with an address-derived pattern, then keep checking
 * it for writes made behind this process's back.  Physical-page reporting
 * through /proc/self/pagemap is optional; the memory check works without it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GIB (UINT64_C(1024) * 1024 * 1024)
#define MAX_WORD_REPORTS 16
#define MAX_PAGE_REPORTS 8

struct checker {
	uint64_t *words;
	size_t word_count;
	uint64_t bad;
	int pagemap_fd;
	uint64_t page_size;
	uintptr_t reported_pages[MAX_PAGE_REPORTS];
	size_t page_report_count;
	int selftest;
	size_t selftest_index;
	uint64_t selftest_value;
	int selftest_detected;
};

static uint64_t pattern(const uint64_t *p)
{
	return ((uint64_t)(uintptr_t)p * UINT64_C(0x9E3779B97F4A7C15)) ^
	       UINT64_C(0x5A5A5A5AA5A5A5A5);
}

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

static int available_memory(uint64_t *bytes)
{
	FILE *file = fopen("/proc/meminfo", "r");
	char line[256];

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		unsigned long long kib;
		if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
			fclose(file);
			if (kib > UINT64_MAX / 1024)
				return -1;
			*bytes = (uint64_t)kib * 1024;
			return 0;
		}
	}
	fclose(file);
	return -1;
}

static int physical_page(const struct checker *checker, uintptr_t va,
			 uint64_t *physical)
{
	uint64_t entry;
	uint64_t pfn;
	off_t offset;

	if (checker->pagemap_fd < 0)
		return 0;
	offset = (off_t)((va / checker->page_size) * sizeof(entry));
	if (pread(checker->pagemap_fd, &entry, sizeof(entry), offset) !=
	    (ssize_t)sizeof(entry))
		return 0;
	if (!(entry & (UINT64_C(1) << 63)))
		return 0;
	pfn = entry & ((UINT64_C(1) << 55) - 1);
	/* Linux hides PFNs from unprivileged readers by returning zero. */
	if (!pfn)
		return 0;
	*physical = pfn * checker->page_size;
	return 1;
}

static void report_page(struct checker *checker, uintptr_t va)
{
	uintptr_t page_va = va & ~(uintptr_t)(checker->page_size - 1);
	uint64_t page_pa;
	size_t i;

	if (checker->page_report_count == MAX_PAGE_REPORTS)
		return;
	for (i = 0; i < checker->page_report_count; i++)
		if (checker->reported_pages[i] == page_va)
			return;

	checker->reported_pages[checker->page_report_count++] = page_va;
	if (physical_page(checker, page_va, &page_pa)) {
		printf("corrupted page: va=%p pa=%#" PRIx64 "\n",
		       (void *)page_va, page_pa);
	} else {
		printf("corrupted page: va=%p pa=unavailable "
		       "(pagemap disabled, restricted, or page not present)\n",
		       (void *)page_va);
	}
}

static uint64_t verify_mapping(struct checker *checker)
{
	uint64_t bad_before = checker->bad;
	size_t i;

	for (i = 0; i < checker->word_count; i++) {
		uint64_t want = pattern(&checker->words[i]);
		uint64_t found = __atomic_load_n(&checker->words[i],
						 __ATOMIC_RELAXED);

		if (found == want)
			continue;
		checker->bad++;
		if (checker->bad <= MAX_WORD_REPORTS) {
			printf("changed: va=%p found=%#" PRIx64
			       " want=%#" PRIx64 "\n",
			       (void *)&checker->words[i], found, want);
		}
		report_page(checker, (uintptr_t)&checker->words[i]);
		if (checker->selftest && i == checker->selftest_index &&
		    found == checker->selftest_value)
			checker->selftest_detected = 1;
		/* Repair it so later rounds can detect later writes too. */
		__atomic_store_n(&checker->words[i], want, __ATOMIC_RELAXED);
	}
	return checker->bad - bad_before;
}

static void *selftest_writer(void *argument)
{
	struct checker *checker = argument;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };

	nanosleep(&delay, NULL);
	__atomic_store_n(&checker->words[checker->selftest_index],
			 checker->selftest_value, __ATOMIC_RELAXED);
	return NULL;
}

static struct timespec deadline_after(uint64_t seconds)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	deadline.tv_sec += (time_t)seconds;
	return deadline;
}

static int deadline_reached_at(const struct timespec *deadline)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	return now.tv_sec > deadline->tv_sec ||
	       (now.tv_sec == deadline->tv_sec &&
		now.tv_nsec >= deadline->tv_nsec);
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--size-gib N] [--seconds N] [--selftest] "
		"[--no-pagemap]\n"
		"       %s [GIB [SECONDS]]\n"
		"Defaults: 2 GiB, 60 seconds.  --selftest expects and reports "
		"one intentional write.\n",
		program, program);
}

int main(int argc, char **argv)
{
	uint64_t size_gib = 2, seconds = 60, size_bytes, available = 0;
	long page_size;
	size_t word_count, i;
	struct timespec deadline;
	struct checker checker = { .pagemap_fd = -1, .page_size = 4096 };
	pthread_t thread;
	int thread_started = 0;
	int no_pagemap = 0;
	uint64_t rounds = 0;
	int option;
	static const struct option options[] = {
		{ "size-gib", required_argument, NULL, 's' },
		{ "seconds", required_argument, NULL, 't' },
		{ "selftest", no_argument, NULL, 1000 },
		{ "no-pagemap", no_argument, NULL, 1001 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((option = getopt_long(argc, argv, "s:t:h", options, NULL)) != -1) {
		switch (option) {
		case 's':
			if (parse_u64(optarg, &size_gib))
				goto bad_arguments;
			break;
		case 't':
			if (parse_u64(optarg, &seconds))
				goto bad_arguments;
			break;
		case 1000:
			checker.selftest = 1;
			break;
		case 1001:
			no_pagemap = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			goto bad_arguments;
		}
	}
	/* Retain the reference test's positional GIB SECONDS interface. */
	if (optind < argc && parse_u64(argv[optind++], &size_gib))
		goto bad_arguments;
	if (optind < argc && parse_u64(argv[optind++], &seconds))
		goto bad_arguments;
	if (optind != argc || !size_gib || !seconds ||
	    size_gib > SIZE_MAX / GIB)
		goto bad_arguments;
	size_bytes = size_gib * GIB;
	word_count = (size_t)size_bytes / sizeof(uint64_t);

	if (available_memory(&available) != 0) {
		fprintf(stderr, "memcheck: cannot read MemAvailable\n");
		return 1;
	}
	printf("memcheck: requested=%" PRIu64 " GiB, MemAvailable=%" PRIu64
	       " MiB\n", size_gib, available / (1024 * 1024));
	/* Leave at least 25% of currently available memory to Linux. */
	if (size_bytes > available - available / 4) {
		fprintf(stderr,
			"memcheck: refusing to use more than 75%% of "
			"MemAvailable; request a smaller size\n");
		return 1;
	}

	checker.words = mmap(NULL, (size_t)size_bytes, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (checker.words == MAP_FAILED) {
		perror("memcheck: mmap");
		return 1;
	}
	(void)mlock(checker.words, (size_t)size_bytes);
	checker.word_count = word_count;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size > 0)
		checker.page_size = (uint64_t)page_size;

	printf("memcheck: filling %zu bytes at %p\n", (size_t)size_bytes,
	       (void *)checker.words);
	for (i = 0; i < word_count; i++)
		__atomic_store_n(&checker.words[i], pattern(&checker.words[i]),
				 __ATOMIC_RELAXED);

	if (!no_pagemap) {
		checker.pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
		if (checker.pagemap_fd < 0)
			fprintf(stderr,
				"memcheck: pagemap unavailable (%s); continuing "
				"without physical addresses\n", strerror(errno));
		else
			printf("memcheck: pagemap enabled (PFN access may still "
			       "be restricted)\n");
	} else {
		printf("memcheck: pagemap disabled by request\n");
	}

	if (checker.selftest) {
		checker.selftest_index = word_count / 2;
		checker.selftest_value =
			pattern(&checker.words[checker.selftest_index]) ^
			UINT64_C(0xDEADBEEF01234567);
		printf("memcheck selftest: a second thread will change va=%p once\n",
		       (void *)&checker.words[checker.selftest_index]);
		if (pthread_create(&thread, NULL, selftest_writer, &checker) != 0) {
			fprintf(stderr, "memcheck: cannot create selftest thread\n");
			munmap(checker.words, (size_t)size_bytes);
			return 1;
		}
		thread_started = 1;
	}

	deadline = deadline_after(seconds);
	do {
		(void)verify_mapping(&checker);
		rounds++;
	} while (!deadline_reached_at(&deadline));

	if (thread_started) {
		pthread_join(thread, NULL);
		/* Guarantee that the delayed intentional write is checked. */
		(void)verify_mapping(&checker);
		rounds++;
	}

	printf("memcheck: %" PRIu64 " GiB, %" PRIu64 " rounds, %" PRIu64
	       " corrupted 64-bit locations\n", size_gib, rounds, checker.bad);
	if (checker.selftest) {
		if (checker.selftest_detected) {
			printf("MEMCHECK SELFTEST: PASS (intentional write detected)\n");
		} else {
			printf("MEMCHECK SELFTEST: FAIL (intentional write missed)\n");
			checker.bad++;
		}
	} else {
		printf("MEMCHECK: %s\n", checker.bad ? "FAIL" : "PASS");
	}

	if (checker.pagemap_fd >= 0)
		close(checker.pagemap_fd);
	munmap(checker.words, (size_t)size_bytes);
	return checker.selftest ? !checker.selftest_detected : checker.bad != 0;

bad_arguments:
	usage(argv[0]);
	return 2;
}
