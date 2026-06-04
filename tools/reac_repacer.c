// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_repacer — protocol-agnostic L2 de-jitter / re-pacing relay (bidirectional,
// multiport).
//
// Sits between the gretap tunnel (Wi-Fi, bursty) and the local REAC stageboxes,
// with every interface unbridged so raw L2 TX works cleanly. Handles one or more
// REAC ports in a single daemon. Each port is an independent (in,out) pair:
//   forward (tunnel -> stagebox): buffers the bursty master broadcast a few ms
//      and re-emits a constant cadence — fills the gaps a clock-slave stagebox
//      cannot tolerate.
//   return  (stagebox -> tunnel): passes the stagebox's frames straight through
//      (the master is the clock owner and tolerates input jitter).
//
// Multiport sync: all ports of one mixer share a single word clock at the source,
// so they share ONE recovered pacing clock here — a single SCHED_FIFO tick emits
// every port on the same period, and the period is re-tuned from the active
// ports' average occupancy drift. Per-port jitter buffers, drops, holds and gap
// concealment keep a burst or underrun on one port from touching the others; only
// the slow clock is common (driven by the active ports), so a stalled port cannot
// drag the healthy ones. The result keeps the stageboxes sample-phase-aligned with
// each other, mirroring the mixer's own single master clock.
//
// It does NOT decode REAC, touch bytes/order, or do VLAN tagging — the kernel's
// .11/.12/.13 subinterfaces still tag the tunnel side; all relay interfaces are
// untagged. PACKET_OUTGOING frames are skipped on both RX paths so we never echo
// our own TX.
//
// Usage: reac_repacer --port reactap.11:lan1 --port reactap.12:lan2
//          --port reactap.13:lan3 [--prefill-ms 8] [--adapt] [--reclaim]
//          [--no-auto-rate] [--period-ns N] [--cpu 3] [--prio 80]
//          [--bcast-only] [--no-plc] [--ctrl-bypass]
//        (single port also accepts the legacy  --in IFACE --out IFACE  form.)

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
#define MAX_STREAMS 8

static volatile sig_atomic_t running = 1;

/* global config (identical for every port) */
static int g_bcast_only, g_bypass, g_ctrl_bypass, g_auto_rate = 1;
static int g_servo_clamp_ppm = 3000;   /* reserved: period clamp width in ppm */
static int g_adapt;                 /* adaptive variable-window: auto-size buffer to burst depth */
static int g_adapt_margin = 10;     /* keep the occ low-water-mark this many slots above 0 */
static int g_adapt_min_ms  = 6;     /* floor latency when shrinking */
static int g_adapt_max_ms  = 120;   /* ceiling latency when growing */
static int g_plc = 1;               /* gap concealment: repeat last frame (next counter) on underrun */
static int g_reclaim;               /* opt-in: actively shrink latency (drops -> clicks). Default grow-only. */

struct iface { int fd; int ifindex; struct sockaddr_ll tx; };

/* one REAC port: its own ifaces, ring, counters and pacing state. The shared
 * clock (period/deadline, below) drives every stream's emit on the same tick. */
struct stream {
	char in_name[IFNAMSIZ], out_name[IFNAMSIZ];
	struct iface IN, OUT;            /* IN = tunnel side, OUT = stagebox side */

	/* SPSC ring: rx thread produces, the shared pacer consumes */
	uint8_t (*slot_buf)[SLOT_SZ];
	uint16_t *slot_len;
	volatile unsigned r_head, r_tail;
	int occ_cap;                    /* runaway safety only */

	/* counters */
	unsigned long long n_rx, n_tx, n_drop_full, n_underrun, n_txerr, n_ret, n_reterr, n_ctrl, n_plc, n_skip, n_hold;
	unsigned long long g_in_frames; /* every accepted input frame, for rate detection */
	unsigned long long last_in;     /* for continuous rate detection */

	/* per-port pacing state (persists across ticks) */
	int target;
	long long occ_sum; int occ_cnt;
	int edit_win, edit_owe, edit_gap;
	int t_floor, t_ceil, occ_min_win, adapt_cnt, gh, sd;
	int started, prefill;           /* per-port activation: detect input, then prefill, then emit */
	unsigned emit_ctr; int last_idx, have_last;
	uint8_t plc_buf[SLOT_SZ];

	pthread_t rx_th, ret_th;
};

static struct stream streams[MAX_STREAMS];
static int n_streams;

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

/* forward RX: tunnel master broadcast -> this stream's ring */
static void *fwd_rx(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	while (recv(s->IN.fd, buf, sizeof buf, MSG_DONTWAIT) > 0) ;   /* flush stale backlog -> no startup overshoot */
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->IN.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 18) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* skip our own return TX */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC ethertype */
		if (g_bcast_only && buf[0] != 0xff) continue;               /* master broadcast only */
		s->g_in_frames++;                                           /* input frame rate (rate detect) */
		/* ALL frame types ride ONE master counter sequence at a constant packet rate:
		 * the control frames (cdea channel-map, cfea announce) occupy counter slots
		 * interspersed with 0000 audio -- they replace audio slots rather than adding
		 * to the stream, so audio+control always sum to the packet rate. Each control
		 * frame also carries audio samples in its slot, so emitting it early
		 * (--ctrl-bypass) puts those samples ahead of cadence: one click per control
		 * frame. Default keeps every type IN SEQUENCE through the ring; in == out, so
		 * latency stays flat even through a control burst. */
		int is_ctrl = !(buf[16] == 0x00 && buf[17] == 0x00);
		if (is_ctrl) {
			s->n_ctrl++;
			if (g_ctrl_bypass) {   /* A/B only: reproduces the heartbeat artifact */
				sendto(s->OUT.fd, buf, (size_t)n, 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx);
				continue;
			}
		}
		unsigned tail = s->r_tail, next = (tail + 1) & RING_MASK;
		unsigned occ_now = (tail - __atomic_load_n(&s->r_head, __ATOMIC_ACQUIRE)) & RING_MASK;
		if (s->occ_cap && (int)occ_now >= s->occ_cap) { s->n_drop_full++; continue; }  /* bound latency */
		if (next == __atomic_load_n(&s->r_head, __ATOMIC_ACQUIRE)) { s->n_drop_full++; continue; }
		memcpy(s->slot_buf[tail], buf, (size_t)n); s->slot_len[tail] = (uint16_t)n;
		__atomic_store_n(&s->r_tail, next, __ATOMIC_RELEASE); s->n_rx++;
	}
	return NULL;
}

/* return relay: stagebox -> tunnel, immediate (master tolerates input jitter) */
static void *ret_relay(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->OUT.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 14) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* never echo the master back -> no loop */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC frames (incl handshake) */
		if (sendto(s->IN.fd, buf, (size_t)n, 0, (struct sockaddr *)&s->IN.tx, sizeof s->IN.tx) < 0) s->n_reterr++;
		else s->n_ret++;
	}
	return NULL;
}

static long nearest_std_pps(double pps) {
	static const long t[] = { 3675, 4000, 8000 };   /* 44.1 / 48 / 96 kHz */
	long best = t[0]; double bd = 1e18;
	for (unsigned i = 0; i < 3; i++) { double d = pps - (double)t[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = t[i]; } }
	return best;
}

/* recompute the per-port window sizes that depend on the slot period */
static void stream_resize(struct stream *s, long period_ns, int prefill) {
	s->edit_win = (int)(1.0e9 / (double)period_ns); if (s->edit_win < 1000) s->edit_win = 1000;
	s->t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns); if (s->t_floor < 4) s->t_floor = 4;
	s->t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns); if (s->t_ceil > RING_SZ - 64) s->t_ceil = RING_SZ - 64;
	s->target = prefill;
	s->prefill = prefill;
	s->started = 0;          /* re-arm per-port activation (prefill before emitting) */
}

/* one port's emit for this tick. The frozen shared clock owns the cadence; here we
 * spread one scheduled frame-edit per tick (drop = latency down, hold = latency up),
 * conceal underruns, and size this port's buffer to its own burst depth. Returns the
 * port's current occupancy; sets *active when the port has real data flowing. */
static int pace_one(struct stream *s, int *active) {
	/* a port that has never received a frame stays dormant: no emit, no underrun
	 * count, no buffer growth. It wakes up cleanly the instant input appears (e.g.
	 * a stagebox patched in after the daemon is already running). */
	if (s->n_rx == 0) { *active = 0; return 0; }
	unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
	int occ = (int)((tail - head) & RING_MASK);
	/* per-port activation: a port that just started receiving (at daemon start or
	 * hot-patched mid-run) prefills its own buffer before emitting, so it joins the
	 * shared clock cleanly instead of underrunning until it fills. */
	if (!s->started) {
		if (occ < s->prefill) { *active = 0; return occ; }
		s->started = 1;
		s->target = s->prefill;
		s->occ_sum = 0; s->occ_cnt = 0; s->edit_owe = 0; s->edit_gap = 0;
		s->occ_min_win = 1 << 30; s->adapt_cnt = 0; s->gh = 0; s->sd = 0;
		fprintf(stderr, "port %s->%s: REAC detected, prefilled -> active\n", s->in_name, s->out_name);
	}
	int do_drop = 0, do_insert = 0;
	if (s->edit_owe != 0 && ++s->edit_gap >= 250) {
		if (s->edit_owe > 0) { if (occ > s->target / 2 + 2) { do_drop = 1; s->edit_owe--; s->edit_gap = 0; } }
		else { do_insert = 1; s->edit_owe++; s->edit_gap = 0; }
	}
	if (do_insert || occ <= 0) {
		/* hold: repeat the last frame's audio under the next counter, consuming nothing -- used
		 * both to raise latency (insert) and to conceal a real underrun. The monotonic emit_ctr
		 * is essential: a verbatim repeat (dup counter) reads as 65535 lost frames at the slave. */
		if (occ <= 0 && !do_insert) s->n_underrun++;
		if (s->have_last) {
			memcpy(s->plc_buf, s->slot_buf[s->last_idx], s->slot_len[s->last_idx]);
			s->plc_buf[14] = s->emit_ctr & 0xFF; s->plc_buf[15] = (s->emit_ctr >> 8) & 0xFF;
			if (sendto(s->OUT.fd, s->plc_buf, s->slot_len[s->last_idx], 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx) < 0) s->n_txerr++;
			else { if (do_insert) s->n_hold++; else s->n_plc++; }
			s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
		}
	} else {
		if (do_drop) { head = (head + 1) & RING_MASK; occ--; s->n_skip++; }   /* discard one frame's audio */
		uint8_t *f = s->slot_buf[head];
		if (!s->have_last) s->emit_ctr = f[14] | (f[15] << 8);                /* seed the counter */
		f[14] = s->emit_ctr & 0xFF; f[15] = (s->emit_ctr >> 8) & 0xFF;        /* own a monotonic output counter */
		if (sendto(s->OUT.fd, f, s->slot_len[head], 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx) < 0) s->n_txerr++;
		s->last_idx = (int)head; s->have_last = 1;
		__atomic_store_n(&s->r_head, (head + 1) & RING_MASK, __ATOMIC_RELEASE); s->n_tx++;
		s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
	}
	/* edit budget from this port's ~1 s average occupancy: a wide deadband so burst
	 * swings (absorbed by the buffer) never trigger an edit, and it idles once occ
	 * sits near target. */
	s->occ_sum += occ; s->occ_cnt++;
	if (s->occ_cnt >= s->edit_win) {
		double err = (double)s->occ_sum / s->occ_cnt - (double)s->target;
		if (err > 32.0 || err < -32.0) {
			s->edit_owe = (int)err;
			if (s->edit_owe >  48) s->edit_owe =  48;
			if (s->edit_owe < -48) s->edit_owe = -48;
		} else s->edit_owe = 0;
		s->occ_sum = 0; s->occ_cnt = 0;
	}
	/* variable window: size this port's buffer to its OBSERVED burst depth. Grow fast
	 * on a low-water dip (RF-safe), then settle; only shrink with --reclaim (drops). */
	if (g_adapt) {
		if (occ < s->occ_min_win) s->occ_min_win = occ;
		if (++s->adapt_cnt >= 4000) {              /* ~1 s */
			int m = g_adapt_margin;
			if (s->gh > 0) s->gh--;
			if (s->sd > 0) s->sd--;
			if (s->occ_min_win < m && s->gh == 0) { s->target += 16; s->gh = 2; s->sd = 60; }
			else if (g_reclaim && s->occ_min_win > m + 6 && s->sd == 0) {
				int sh = 1 + (s->occ_min_win - m) / 4; if (sh > 16) sh = 16;
				s->target -= sh;
			}
			if (s->target < s->t_floor) s->target = s->t_floor;
			if (s->target > s->t_ceil)  s->target = s->t_ceil;
			s->occ_min_win = 1 << 30; s->adapt_cnt = 0;
		}
	}
	*active = s->have_last;
	return occ;
}

/* reset a port's pacing state and drop its ring (used on a live rate change) */
static void stream_relock(struct stream *s, long period_ns, int prefill) {
	stream_resize(s, period_ns, prefill);
	s->occ_sum = 0; s->occ_cnt = 0; s->edit_owe = 0; s->edit_gap = 0;
	s->occ_min_win = 1 << 30; s->adapt_cnt = 0; s->gh = 0; s->sd = 0;
	__atomic_store_n(&s->r_head, __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

/* sum input frames across all ports (rate detect uses the busiest port's rate; all
 * ports of one mixer share a rate, so any active port gives the answer) */
static double measure_pps(unsigned long long *base, double secs) {
	double best = 0;
	for (int i = 0; i < n_streams; i++) {
		double p = secs > 0 ? (double)(streams[i].g_in_frames - base[i]) / secs : 0;
		if (p > best) best = p;
	}
	return best;
}

static int add_stream(const char *in, const char *out) {
	if (n_streams >= MAX_STREAMS) { fprintf(stderr, "too many ports (max %d)\n", MAX_STREAMS); return -1; }
	struct stream *s = &streams[n_streams];
	strncpy(s->in_name, in, IFNAMSIZ - 1);
	strncpy(s->out_name, out, IFNAMSIZ - 1);
	n_streams++;
	return 0;
}

int main(int argc, char **argv) {
	const char *pend_in = NULL, *pend_out = NULL;
	int prefill_ms = 12, cpu = 3, prio = 80; long period_ns = 250000;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--in") && i + 1 < argc) pend_in = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) pend_out = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			char *spec = argv[++i], *colon = strchr(spec, ':');
			if (!colon) { fprintf(stderr, "--port needs IN:OUT, got '%s'\n", spec); return 2; }
			*colon = '\0';
			if (add_stream(spec, colon + 1)) return 2;
		}
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
		else if (!strcmp(argv[i], "--reclaim")) g_reclaim = 1;
	}
	if (pend_in && pend_out) add_stream(pend_in, pend_out);   /* legacy single-port form */
	if (n_streams == 0) add_stream("reactap.11", "lan1");     /* default */

	int prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
	if (prefill < 1) prefill = 1;
	if (prefill > RING_SZ - 2) prefill = RING_SZ - 2;

	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

	/* allocate + open every port */
	for (int i = 0; i < n_streams; i++) {
		struct stream *s = &streams[i];
		s->slot_buf = calloc(RING_SZ, SLOT_SZ);
		s->slot_len = calloc(RING_SZ, sizeof(uint16_t));
		if (!s->slot_buf || !s->slot_len) { perror("calloc"); return 1; }
		s->occ_cap = RING_SZ - 64;        /* runaway safety only */
		s->last_in = 0;
		if (open_iface(s->in_name, &s->IN) || open_iface(s->out_name, &s->OUT)) return 1;
		stream_resize(s, period_ns, prefill);
		s->occ_min_win = 1 << 30;
	}
	mlockall(MCL_CURRENT | MCL_FUTURE);

	for (int i = 0; i < n_streams; i++) {
		pthread_create(&streams[i].rx_th, NULL, fwd_rx, &streams[i]);
		pthread_create(&streams[i].ret_th, NULL, ret_relay, &streams[i]);
	}

	/* one RT pacing thread (this one) on a dedicated core, isolated from the NIC IRQ core */
	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); sched_setaffinity(0, sizeof set, &set);
	struct sched_param sp = { .sched_priority = prio };
	if (syscall(__NR_sched_setscheduler, 0, SCHED_FIFO, &sp) != 0) perror("sched_setscheduler");
	int pm = open("/dev/cpu_dma_latency", O_WRONLY);
	if (pm >= 0) { int z = 0; if (write(pm, &z, sizeof z) < 0) perror("cpu_dma_latency"); }

	if (g_auto_rate) {
		/* Detect the sample rate from the input packet rate: a REAC stream emits one
		 * frame per slot, so the busiest port's measured pps gives the slot period.
		 * Snap to the nearest standard rate when close, else use the measured value. */
		unsigned long long base[MAX_STREAMS];
		for (int i = 0; i < n_streams; i++) base[i] = streams[i].g_in_frames;
		long long t0 = ns_now();
		struct timespec ms = { 2, 0 }; nanosleep(&ms, NULL);
		double secs = (double)(ns_now() - t0) / 1.0e9;
		double pps = measure_pps(base, secs);
		if (pps > 100.0) {
			long snap = nearest_std_pps(pps);
			double err = pps - (double)snap; if (err < 0) err = -err;
			double chosen = (err < (double)snap * 0.08) ? (double)snap : pps;
			period_ns = (long)(1.0e9 / pps + 0.5);    /* freeze at the MEASURED rate */
			prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
			if (prefill < 1) prefill = 1;
			if (prefill > RING_SZ - 2) prefill = RING_SZ - 2;
			for (int i = 0; i < n_streams; i++) stream_resize(&streams[i], period_ns, prefill);
			fprintf(stderr, "auto-rate: %.0f pps measured (%.1f kHz), period frozen at %ld ns\n",
			        pps, chosen * 12.0 / 1000.0, period_ns);
		} else {
			fprintf(stderr, "auto-rate: no input seen on any port; keeping period=%ld ns\n", period_ns);
		}
		/* drop whatever overfilled during the measurement window -> prefill fresh */
		for (int i = 0; i < n_streams; i++)
			__atomic_store_n(&streams[i].r_head, __atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	}

	fprintf(stderr, "repacer(multiport): %d port(s) prefill=%d (%d ms) period=%ld ns cpu=%d bcast_only=%d adapt=%d(%d-%dms) plc=%d reclaim=%d\n",
	        n_streams, prefill, prefill_ms, period_ns, cpu, g_bcast_only, g_adapt, g_adapt_min_ms, g_adapt_max_ms, g_plc, g_reclaim);
	for (int i = 0; i < n_streams; i++)
		fprintf(stderr, "  port %d: %s -> %s\n", i, streams[i].in_name, streams[i].out_name);

	/* pace once EVERY actively-receiving port has prefilled (not just the first) -- otherwise
	 * a port that fills slower than its peers starts pacing under-filled and underruns at
	 * startup. Ports with no input yet (n_rx==0) don't block the start; they join cleanly
	 * when their data arrives. */
	while (running) {
		int any_active = 0, all_ready = 1;
		for (int i = 0; i < n_streams; i++) {
			if (streams[i].n_rx == 0) continue;          /* idle port: not required to start */
			any_active = 1;
			unsigned occ = (__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK;
			if ((int)occ < prefill) all_ready = 0;
		}
		if (any_active && all_ready) break;
		struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
	}
	fprintf(stderr, "repacer: prefilled, pacing\n");

	long long deadline = ns_now() + period_ns; double period = (double)period_ns;
	long long last = ns_now();
	/* shared slow period re-tune: nudge the one frozen period to null the active ports'
	 * mean occupancy drift. Driven by the active ports only, so a stalled port can't pull
	 * the shared clock. ~8 s-spaced, tiny, bounded nudges -> nails the true source rate
	 * with steady-state edits ~0 and no wander. */
	int retune_win = (int)(8.0e9 / (double)period_ns);
	if (retune_win < 8000) retune_win = 8000;
	int retune_cnt = 0; double retune_occ_sum = 0; double retune_avg0 = (double)prefill;
	int rate_chg = 0;

	while (running) {
		struct timespec d = { deadline / 1000000000LL, deadline % 1000000000LL };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);

		/* emit every port on this shared tick; collect the active ports' mean occupancy */
		double occ_acc = 0; int nact = 0; int show_occ = 0;
		for (int i = 0; i < n_streams; i++) {
			int active = 0;
			int occ = pace_one(&streams[i], &active);
			if (i == 0) show_occ = occ;
			if (active) { occ_acc += occ; nact++; }
		}
		double avg_occ = nact ? occ_acc / nact : (double)retune_avg0;

		retune_occ_sum += avg_occ;
		if (++retune_cnt >= retune_win) {
			double avg = retune_occ_sum / retune_cnt;
			double dd = avg - retune_avg0;
			retune_avg0 = avg;
			if (dd > 1.0 || dd < -1.0) {
				double n = period * dd / (double)retune_win; if (n > 8.0) n = 8.0; if (n < -8.0) n = -8.0;
				period -= n;
				if (period < (double)period_ns * 0.997) period = (double)period_ns * 0.997;
				if (period > (double)period_ns * 1.003) period = (double)period_ns * 1.003;
			}
			retune_occ_sum = 0; retune_cnt = 0;
		}

		deadline += (long long)(period + 0.5);
		long long now = ns_now();
		if (now - last > 1000000000LL) {
			/* continuous rate detection: a SAMPLE-RATE change shifts the busiest port's input
			 * pps far from the pacing rate (a clock offset is <0.3%, a 44.1<->48<->96 k change is
			 * 9-100%). Re-lock every port at the new rate instead of draining. */
			double rsecs = (double)(now - last) / 1.0e9;
			unsigned long long inb[MAX_STREAMS];
			for (int i = 0; i < n_streams; i++) inb[i] = streams[i].last_in;
			double in_pps = measure_pps(inb, rsecs);
			for (int i = 0; i < n_streams; i++) streams[i].last_in = streams[i].g_in_frames;
			long cand = nearest_std_pps(in_pps);
			double cerr = (in_pps >= (double)cand) ? (in_pps - cand) / cand : (cand - in_pps) / cand;
			int is_change = (g_auto_rate && in_pps > 2000.0 && cerr < 0.03 && cand != nearest_std_pps(1.0e9 / period_ns));
			if (is_change) rate_chg++; else rate_chg = 0;
			if (rate_chg >= 3) {   /* a standard rate sustained ~3 s -> a real (rare) change */
				rate_chg = 0; period_ns = (long)(1.0e9 / (double)cand + 0.5); period = (double)period_ns;
				prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
				if (prefill < 1) prefill = 1;
				if (prefill > RING_SZ - 2) prefill = RING_SZ - 2;
				retune_win = (int)(8.0e9 / (double)period_ns); if (retune_win < 8000) retune_win = 8000;
				retune_cnt = 0; retune_occ_sum = 0; retune_avg0 = (double)prefill;
				for (int i = 0; i < n_streams; i++) {
					streams[i].have_last = 0;
					stream_relock(&streams[i], period_ns, prefill);
				}
				while (running) {
					int ready = 0;
					for (int i = 0; i < n_streams; i++) { int o = (int)((__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK); if (o >= prefill) { ready = 1; break; } }
					if (ready) break;
					struct timespec rt = { 0, 1000000 }; nanosleep(&rt, NULL);
				}
				for (int i = 0; i < n_streams; i++) streams[i].last_in = streams[i].g_in_frames;
				fprintf(stderr, "auto-rate: input rate changed to %.0f pps (%.1f kHz) -> re-locked all ports, period=%ld ns\n", in_pps, in_pps * 12.0 / 1000.0, period_ns);
				deadline = ns_now() + period_ns; last = ns_now(); continue;
			}
			/* per-port telemetry line */
			fprintf(stderr, "per=%.0f", period);
			for (int i = 0; i < n_streams; i++) {
				struct stream *s = &streams[i];
				unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
				int occ = (int)((tail - head) & RING_MASK);
				fprintf(stderr, " | %s occ=%d(%dms) tgt=%d skip=%llu hold=%llu under=%llu plc=%llu rx=%llu",
				        s->out_name, occ, (int)((long long)occ * period_ns / 1000000), s->target,
				        s->n_skip, s->n_hold, s->n_underrun, s->n_plc, s->n_rx);
			}
			fprintf(stderr, "\n");
			(void)show_occ;
			last = now;
		}
	}
	for (int i = 0; i < n_streams; i++)
		fprintf(stderr, "repacer: stop %s->%s rx=%llu tx=%llu txerr=%llu under=%llu | ret=%llu reterr=%llu\n",
		        streams[i].in_name, streams[i].out_name, streams[i].n_rx, streams[i].n_tx, streams[i].n_txerr, streams[i].n_underrun, streams[i].n_ret, streams[i].n_reterr);
	return 0;
}
