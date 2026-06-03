// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// REAC re-pacer feasibility gate.
//
// The re-pacer must emit one frame every 250 us with low jitter on a router
// that is simultaneously forwarding REAC. The kernel TSN scheduler (sch_etf)
// is not available in the OpenWrt feed, so the emit timer is userspace
// clock_nanosleep under SCHED_FIFO. This probe measures whether that loop can
// actually hold 250 us on this hardware/kernel BEFORE the full daemon is built.
//
// Pure scheduling probe: it sleeps to accumulated absolute deadlines and
// records how late each wake-up was. No packets are sent (the TX syscall adds
// a little on top, validated end-to-end later). Run it while the rig forwards
// live REAC so the measurement reflects real load.
//
// Usage: pacer_probe [period_ns] [iterations] [cpu] [prio]
//   defaults: 250000 ns, 40000 iters (~10 s), cpu 3, prio 80

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

static long long ns_of(const struct timespec *t)
{
	return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	long period_ns = (argc > 1) ? atol(argv[1]) : 250000;
	int n          = (argc > 2) ? atoi(argv[2]) : 40000;
	int cpu        = (argc > 3) ? atoi(argv[3]) : 3;
	int prio       = (argc > 4) ? atoi(argv[4]) : 80;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof set, &set) != 0)
		perror("sched_setaffinity");

	struct sched_param sp = { .sched_priority = prio };
	/* musl stubs sched_setscheduler() to ENOSYS (POSIX process-scope vs Linux
	 * per-thread mismatch); invoke the syscall directly to actually get RT. */
	if (syscall(__NR_sched_setscheduler, 0, SCHED_FIFO, &sp) != 0)
		perror("sched_setscheduler");

	/* Keep the CPU out of deep idle: waking from a deep C-state adds tens of us
	 * of variable latency to clock_nanosleep. Holding cpu_dma_latency=0 open
	 * caps PM-QoS wake latency at 0 for the duration of the probe. */
	int pmqos = open("/dev/cpu_dma_latency", O_WRONLY);
	if (pmqos >= 0) {
		int z = 0;
		if (write(pmqos, &z, sizeof z) < 0)
			perror("cpu_dma_latency");
	} /* leave the fd open on purpose */

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		perror("mlockall");

	long long *late = malloc((size_t)n * sizeof *late);
	if (!late) { perror("malloc"); return 1; }

	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	long long deadline = ns_of(&t) + period_ns;

	for (int i = 0; i < n; i++) {
		struct timespec d = { deadline / 1000000000LL, deadline % 1000000000LL };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		late[i] = ns_of(&now) - deadline;   /* wake lateness, ns */
		deadline += period_ns;              /* accumulate: drift-free */
	}

	double mean = 0;
	for (int i = 0; i < n; i++) mean += (double)late[i];
	mean /= n;
	double var = 0;
	long long mx = late[0], mn = late[0];
	int o50 = 0, o100 = 0, o250 = 0;
	for (int i = 0; i < n; i++) {
		double dv = (double)late[i] - mean;
		var += dv * dv;
		if (late[i] > mx) mx = late[i];
		if (late[i] < mn) mn = late[i];
		if (late[i] > 50000)  o50++;
		if (late[i] > 100000) o100++;
		if (late[i] > 250000) o250++;     /* missed an entire 250 us slot */
	}
	double sd = sqrt(var / n);
	qsort(late, n, sizeof *late, cmp_ll);
	long long p99  = late[(int)(0.99 * n)];
	long long p999 = late[(int)(0.999 * n)];

	printf("SCHED_FIFO clock_nanosleep pacing: period=%ld ns, n=%d, cpu=%d, prio=%d\n",
	       period_ns, n, cpu, prio);
	printf("  wake lateness ns: mean %.0f  sd %.0f  min %lld  p99 %lld  p99.9 %lld  max %lld\n",
	       mean, sd, mn, p99, p999, mx);
	printf("  >50us: %d (%.3f%%)   >100us: %d (%.3f%%)   >250us(missed slot): %d (%.4f%%)\n",
	       o50, 100.0 * o50 / n, o100, 100.0 * o100 / n, o250, 100.0 * o250 / n);
	printf("  VERDICT: %s\n",
	       (sd < 30000.0 && o100 < n / 1000 + 1) ?
	       "FEASIBLE for 250us re-pace" :
	       "MARGINAL -> needs IRQ isolation / PREEMPT_RT");

	free(late);
	return 0;
}
