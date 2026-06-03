// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_repacer — protocol-agnostic L2 de-jitter / re-pacing relay (bidirectional).
//
// Sits between the gretap tunnel (Wi-Fi, bursty) and the local REAC stagebox,
// with BOTH interfaces unbridged so raw L2 TX works cleanly:
//   forward (tunnel -> stagebox): buffers the bursty master broadcast a few ms
//      and re-emits a constant ~250us cadence (clock-recovery servo) — fills the
//      gaps a clock-slave stagebox cannot tolerate.
//   return  (stagebox -> tunnel): passes the stagebox's frames straight through
//      (the master is the clock owner and tolerates input jitter).
// It does NOT decode REAC, touch bytes/order, or do VLAN tagging — the kernel's
// .11 subinterface still tags the tunnel side; both relay interfaces are untagged.
// PACKET_OUTGOING frames are skipped on both RX paths so we never echo our own TX.
//
// Usage: reac_repacer --in reactap.11 --out lan1 [--prefill-ms 12]
//        [--period-ns 250000] [--cpu 3] [--prio 80] [--bcast-only] [--bypass]
//        [--ctrl-bypass]

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#ifndef PACKET_QDISC_BYPASS
#define PACKET_QDISC_BYPASS 20
#endif

#define RING_BITS 11
#define RING_SZ   (1 << RING_BITS)
#define RING_MASK (RING_SZ - 1)
#define SLOT_SZ   1600

static uint8_t  slot_buf[RING_SZ][SLOT_SZ];
static uint16_t slot_len[RING_SZ];
static volatile unsigned r_head, r_tail;           /* SPSC: fwd_rx produces, main consumes */

static volatile sig_atomic_t running = 1;
static unsigned long long n_rx, n_tx, n_drop_full, n_underrun, n_txerr, n_ret, n_reterr, n_ctrl, n_plc;
static unsigned long long g_in_frames;     /* every accepted input frame, for startup rate detection */
static int g_bcast_only, g_bypass, g_ctrl_bypass, g_auto_rate = 1;
static int g_servo_clamp_ppm = 3000;   /* servo period clamp (was a hardcoded ±500ppm,
                                        * too tight to reach the master's true rate) */
static int g_adapt;                 /* adaptive variable-window: auto-size buffer to burst depth */
static int g_adapt_margin = 10;     /* keep the occ low-water-mark this many slots above 0 */
static int g_adapt_min_ms  = 6;     /* floor latency when shrinking */
static int g_adapt_max_ms  = 120;   /* ceiling latency when growing */
static int g_plc = 1;               /* gap concealment: repeat last frame (next counter) on underrun */
static int g_occ_cap;               /* hard cap on audio buffer occupancy -> bounds latency */

struct iface { int fd; int ifindex; struct sockaddr_ll tx; };
static struct iface IN, OUT;        /* IN = tunnel side (reactap.11), OUT = stagebox side (lan1) */

static void on_sig(int s) { (void)s; running = 0; }

static long long ns_now(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int open_iface(const char *name, struct iface *o) {
	int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (s < 0) { perror("socket"); return -1; }
	struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror(name); close(s); return -1; }
	struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET; sll.sll_protocol = htons(ETH_P_ALL); sll.sll_ifindex = ifr.ifr_ifindex;
	if (bind(s, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); close(s); return -1; }
	int rcv = 8 * 1024 * 1024; setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
	if (g_bypass) { int one = 1; setsockopt(s, SOL_PACKET, PACKET_QDISC_BYPASS, &one, sizeof one); }
	o->fd = s; o->ifindex = ifr.ifr_ifindex;
	memset(&o->tx, 0, sizeof o->tx);
	o->tx.sll_family = AF_PACKET; o->tx.sll_ifindex = ifr.ifr_ifindex; o->tx.sll_halen = 6;
	return 0;
}

/* forward RX: tunnel master broadcast -> ring */
static void *fwd_rx(void *arg) {
	(void)arg; uint8_t buf[SLOT_SZ];
	while (recv(IN.fd, buf, sizeof buf, MSG_DONTWAIT) > 0) ;   /* flush stale backlog -> no startup overshoot */
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(IN.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 18) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* skip our own return TX */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC ethertype */
		if (g_bcast_only && buf[0] != 0xff) continue;               /* master broadcast only */
		g_in_frames++;                                              /* input frame rate (startup detect) */
		/* ALL frame types ride ONE master counter sequence at a constant packet rate:
		 * the control frames (cdea channel-map, cfea announce) occupy counter slots
		 * interspersed with 0000 audio -- they replace audio slots rather than adding
		 * to the stream, so audio+control always sum to the packet rate. Each control
		 * frame also carries audio samples in its slot, so emitting it early
		 * (--ctrl-bypass) puts those samples ahead of cadence: one click per control
		 * frame -- the periodic heartbeat at lock, a denser burst during connection
		 * setup. Default keeps every type IN SEQUENCE through the ring; in == out, so
		 * latency stays flat even through a control burst. */
		int is_ctrl = !(buf[16] == 0x00 && buf[17] == 0x00);
		if (is_ctrl) {
			n_ctrl++;
			if (g_ctrl_bypass) {   /* A/B only: reproduces the heartbeat artifact */
				sendto(OUT.fd, buf, (size_t)n, 0, (struct sockaddr *)&OUT.tx, sizeof OUT.tx);
				continue;
			}
		}
		unsigned tail = r_tail, next = (tail + 1) & RING_MASK;
		unsigned occ_now = (tail - __atomic_load_n(&r_head, __ATOMIC_ACQUIRE)) & RING_MASK;
		if (g_occ_cap && (int)occ_now >= g_occ_cap) { n_drop_full++; continue; }  /* bound latency (no overshoot) */
		if (next == __atomic_load_n(&r_head, __ATOMIC_ACQUIRE)) { n_drop_full++; continue; }
		memcpy(slot_buf[tail], buf, (size_t)n); slot_len[tail] = (uint16_t)n;
		__atomic_store_n(&r_tail, next, __ATOMIC_RELEASE); n_rx++;
	}
	return NULL;
}

/* return relay: stagebox -> tunnel, immediate (master tolerates input jitter) */
static void *ret_relay(void *arg) {
	(void)arg; uint8_t buf[SLOT_SZ];
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(OUT.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 14) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* never echo the master back -> no loop */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC frames (incl handshake) */
		if (sendto(IN.fd, buf, (size_t)n, 0, (struct sockaddr *)&IN.tx, sizeof IN.tx) < 0) n_reterr++;
		else n_ret++;
	}
	return NULL;
}

int main(int argc, char **argv) {
	const char *in = "reactap.11", *out = "lan1";
	int prefill_ms = 12, cpu = 3, prio = 80; long period_ns = 250000;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--in") && i + 1 < argc) in = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--prefill-ms") && i + 1 < argc) prefill_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--period-ns") && i + 1 < argc) { period_ns = atol(argv[++i]); g_auto_rate = 0; }
		else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bcast-only")) g_bcast_only = 1;
		else if (!strcmp(argv[i], "--bypass")) g_bypass = 1;
		else if (!strcmp(argv[i], "--ctrl-bypass")) g_ctrl_bypass = 1;
		else if (!strcmp(argv[i], "--servo-clamp-ppm") && i + 1 < argc) g_servo_clamp_ppm = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt")) g_adapt = 1;
		else if (!strcmp(argv[i], "--adapt-margin") && i + 1 < argc) g_adapt_margin = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt-min-ms") && i + 1 < argc) g_adapt_min_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt-max-ms") && i + 1 < argc) g_adapt_max_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-plc")) g_plc = 0;
		else if (!strcmp(argv[i], "--no-auto-rate")) g_auto_rate = 0;
	}
	int prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
	if (prefill < 1) prefill = 1;
	if (prefill > RING_SZ - 2) prefill = RING_SZ - 2;
	g_occ_cap = RING_SZ - 64;        /* runaway safety only — never drop audio in normal operation */

	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	if (open_iface(in, &IN) || open_iface(out, &OUT)) return 1;
	mlockall(MCL_CURRENT | MCL_FUTURE);

	pthread_t a, b;
	pthread_create(&a, NULL, fwd_rx, NULL);
	pthread_create(&b, NULL, ret_relay, NULL);

	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); sched_setaffinity(0, sizeof set, &set);
	struct sched_param sp = { .sched_priority = prio };
	if (syscall(__NR_sched_setscheduler, 0, SCHED_FIFO, &sp) != 0) perror("sched_setscheduler");
	int pm = open("/dev/cpu_dma_latency", O_WRONLY);
	if (pm >= 0) { int z = 0; if (write(pm, &z, sizeof z) < 0) perror("cpu_dma_latency"); }

	if (g_auto_rate) {
		/* Detect the sample rate from the input packet rate: a REAC stream emits one
		 * frame per slot, so the measured input pps gives the slot period directly. Count
		 * over a couple of seconds (bursts average out), snap to the nearest standard rate
		 * when close, else use the measured value verbatim. Set --period-ns to override. */
		unsigned long long f0 = g_in_frames; long long t0 = ns_now();
		struct timespec ms = { 2, 0 }; nanosleep(&ms, NULL);
		double secs = (double)(ns_now() - t0) / 1.0e9;
		double pps = secs > 0 ? (double)(g_in_frames - f0) / secs : 0;
		if (pps > 100.0) {
			static const long std_pps[] = { 3675, 4000, 8000 };   /* 44.1 / 48 / 96 kHz */
			long snap = std_pps[0]; double best = 1e18;
			for (unsigned i = 0; i < sizeof std_pps / sizeof std_pps[0]; i++) {
				double d = pps - (double)std_pps[i]; if (d < 0) d = -d;
				if (d < best) { best = d; snap = std_pps[i]; }
			}
			long chosen = (best < (double)snap * 0.08) ? snap : (long)(pps + 0.5);
			period_ns = (long)(1.0e9 / (double)chosen + 0.5);
			prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
			if (prefill < 1) prefill = 1;
			if (prefill > RING_SZ - 2) prefill = RING_SZ - 2;
			fprintf(stderr, "auto-rate: %.0f pps measured -> %ld pps (%.1f kHz), period=%ld ns\n",
			        pps, chosen, (double)chosen * 12.0 / 1000.0, period_ns);
		} else {
			fprintf(stderr, "auto-rate: no input seen; keeping period=%ld ns\n", period_ns);
		}
		/* the ring overfilled during the measurement window -> drop it and prefill fresh */
		__atomic_store_n(&r_head, __atomic_load_n(&r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	}

	fprintf(stderr, "repacer(bidir): in=%s out=%s prefill=%d (%d ms) period=%ld ns cpu=%d bcast_only=%d ctrl_bypass=%d servo_clamp=%dppm adapt=%d(margin=%d %d-%dms) plc=%d\n",
	        in, out, prefill, prefill_ms, period_ns, cpu, g_bcast_only, g_ctrl_bypass, g_servo_clamp_ppm, g_adapt, g_adapt_margin, g_adapt_min_ms, g_adapt_max_ms, g_plc);

	while (running) {
		unsigned occ = (__atomic_load_n(&r_tail, __ATOMIC_ACQUIRE) - r_head) & RING_MASK;
		if ((int)occ >= prefill) break;
		struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
	}
	fprintf(stderr, "repacer: prefilled, pacing\n");

	long long deadline = ns_now() + period_ns; double period = (double)period_ns;
	int target = prefill; long long last = ns_now();
	long long occ_sum = 0; int occ_cnt = 0; double prev_avg = (double)prefill;
	int servo_win = (int)(12.0e9 / (double)period_ns);   /* ~12 s of frames at ANY sample rate */
	if (servo_win < 4000) servo_win = 4000;
	/* adaptive variable-window state: target tracks the observed burst depth */
	int t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns);
	int t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns);
	if (t_floor < 4) t_floor = 4;
	if (t_ceil > RING_SZ - 64) t_ceil = RING_SZ - 64;
	int occ_min_win = 1 << 30, adapt_cnt = 0, calm = 0;
	/* PLC / output-counter state: the re-pacer owns a monotonic emit_ctr */
	unsigned emit_ctr = 0; int last_idx = 0, have_last = 0;
	uint8_t plc_buf[SLOT_SZ];
	while (running) {
		struct timespec d = { deadline / 1000000000LL, deadline % 1000000000LL };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
		unsigned head = r_head, tail = __atomic_load_n(&r_tail, __ATOMIC_ACQUIRE);
		int occ = (int)((tail - head) & RING_MASK);
		if (occ > 0) {
			uint8_t *f = slot_buf[head];
			if (g_plc) {                                  /* own a monotonic output counter */
				if (!have_last) emit_ctr = f[14] | (f[15] << 8);   /* seed from first frame */
				f[14] = emit_ctr & 0xFF; f[15] = (emit_ctr >> 8) & 0xFF;
			}
			if (sendto(OUT.fd, f, slot_len[head], 0, (struct sockaddr *)&OUT.tx, sizeof OUT.tx) < 0) n_txerr++;
			last_idx = (int)head; have_last = 1;
			__atomic_store_n(&r_head, (head + 1) & RING_MASK, __ATOMIC_RELEASE); n_tx++;
			emit_ctr = (emit_ctr + 1) & 0xFFFF;
		} else {
			n_underrun++;
			/* gap concealment: re-send the last frame's audio with the NEXT counter, so
			 * the stagebox still gets a packet every 250 us (clock stays locked) and hears
			 * a brief HOLD instead of a click. The monotonic emit_ctr is essential — a
			 * verbatim repeat (dup counter) reads as 65535 lost frames at the slave. */
			if (g_plc && have_last) {
				memcpy(plc_buf, slot_buf[last_idx], slot_len[last_idx]);
				plc_buf[14] = emit_ctr & 0xFF; plc_buf[15] = (emit_ctr >> 8) & 0xFF;
				if (sendto(OUT.fd, plc_buf, slot_len[last_idx], 0, (struct sockaddr *)&OUT.tx, sizeof OUT.tx) < 0) n_txerr++;
				else n_plc++;
				emit_ctr = (emit_ctr + 1) & 0xFFFF;
			}
		}
		/* Clock-recovery servo: trim the emit period to hold buffer occupancy at
		 * target. Gain-scheduled — a startup over/under-fill drains FAST (coarse
		 * 30 ns step when >12 slots off) while a ±4-slot DEADBAND near target holds
		 * the period dead still. A stationary period = a stable recovered word-clock
		 * for the stagebox PLL (no wander — the ±2% servo's failure mode). The wide
		 * ppm clamp lets the period reach the master's TRUE rate; the old ±500 ppm
		 * clamp couldn't, so occ never converged and the achieved latency sat tens
		 * of ms above the prefill setting. */
		occ_sum += occ; occ_cnt++;
		if (occ_cnt >= servo_win) {                  /* ~12 s window: long enough that burst noise
		                                              * averages out and the drift estimate ~= the
		                                              * TRUE rate mismatch. */
			double avg = (double)occ_sum / occ_cnt;
			double drift = avg - prev_avg;           /* net occ change over the window = rate mismatch */
			/* Cancel only a REAL rate mismatch (drift beyond the burst-noise floor); otherwise
			 * leave the period FROZEN. A stationary period = a stationary recovered word-clock =
			 * zero wander. No level term: occ is allowed to float (a steady clock matters far more
			 * than hitting an exact occ; the buffer + PLC absorb the swings). RATE-INDEPENDENT:
			 * the correction is the pure fractional drift over the window (drift/window_frames),
			 * the period self-scales — no hardcoded packet rate, so it's correct at 44.1/48/96 k. */
			if (drift > 3.0 || drift < -3.0)
				period -= period * drift / (double)servo_win;
			prev_avg = avg;
			double span = (double)period_ns / 1000000.0 * (double)g_servo_clamp_ppm;
			double lo = period_ns - span, hi = period_ns + span;
			if (period < lo) period = lo;
			if (period > hi) period = hi;
			occ_sum = 0; occ_cnt = 0;
		}
		/* Variable window: size the buffer to the OBSERVED burst depth. Track the occ
		 * low-water-mark over ~1 s; if it dips into the safety margin, grow the target
		 * fast (cover the dip); if it stays comfortably high, shrink slowly toward
		 * minimal latency. Self-tunes to the live RF — no fixed guess. The servo above
		 * then drives occ to this moving target. */
		if (g_adapt) {
			if (occ < occ_min_win) occ_min_win = occ;
			if (++adapt_cnt >= 4000) {                 /* ~1 s */
				int m = g_adapt_margin;
				if (occ_min_win < m) { target += 2 * (m - occ_min_win) + 4; calm = 0; }  /* grow fast */
				else if (occ_min_win > m + 16) { if (++calm >= 3) { target -= 2; calm = 0; } }  /* shrink slow */
				else calm = 0;
				if (target < t_floor) target = t_floor;
				if (target > t_ceil)  target = t_ceil;
				occ_min_win = 1 << 30; adapt_cnt = 0;
			}
		}
		deadline += (long long)(period + 0.5);
		long long now = ns_now();
		if (now - last > 2000000000LL) {
			fprintf(stderr, "  fwd rx=%llu tx=%llu occ=%d tgt=%d(%dms) per=%.0f drop=%llu under=%llu plc=%llu txerr=%llu | ctrl=%llu ret=%llu reterr=%llu\n",
			        n_rx, n_tx, occ, target, (int)((long long)target * period_ns / 1000000), period, n_drop_full, n_underrun, n_plc, n_txerr, n_ctrl, n_ret, n_reterr);
			last = now;
		}
	}
	fprintf(stderr, "repacer: stop. fwd rx=%llu tx=%llu txerr=%llu | ret=%llu reterr=%llu\n",
	        n_rx, n_tx, n_txerr, n_ret, n_reterr);
	return 0;
}
