// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* apply_hot_params() — the glitch-free occupancy retarget of a live depth change.
 *
 * A change of the target depth must not flush the ring: the daemon biases its clock
 * for a short walk so the live occupancy glides to the new depth. Four properties
 * are pinned, on the daemon's own function (this file includes tools/reac_repacer.c
 * with main renamed, as test_inject.c does):
 *
 *   - grow holds the output back (negative bias), shrink runs it ahead (positive);
 *   - the bias never exceeds 2000 ppm (RETARGET_MAXPPM), a large jump stretching the walk;
 *   - simulating the armed walk (each biased tick moves the occupancy by
 *     -ppm * 1e-6 slots) lands the occupancy on the new target;
 *   - an unchanged depth arms no walk.
 *
 * Moved here from reac-aes67's tests/test_repacer_retarget.c, which asserted the same
 * four properties on a hand copy of this function and so passed whatever this code did.
 */

#define main repacer_main
#include "../tools/reac_repacer.c"
#undef main

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define P96K 125000L   /* ~8000 frames/s: 8 slots per millisecond */

struct walk { double ppm; int ticks; int new_pf; };

/* run apply_hot_params from a live depth of old_pf slots to prefill_ms */
static struct walk retarget(int old_pf, int prefill_ms)
{
	int prefill = old_pf, t_floor = 0, t_ceil = 0;
	double shtgt = old_pf, max_ppm = 0, ppm = 99, newppm;
	int ticks = 99;
	struct pacer_live L = {
		.prefill_ms = 0, .period_ns = P96K, .prefill = &prefill, .shtgt = &shtgt,
		.t_floor = &t_floor, .t_ceil = &t_ceil, .max_ppm = &max_ppm,
		.retarget_ppm = &ppm, .retarget_ticks = &ticks,
	};
	struct rp_hot in = { .prefill_ms = prefill_ms, .adapt_min_ms = 10, .adapt_max_ms = 300,
	                     .adapt_margin = 8, .servo_clamp_ppm = 30 };
	apply_hot_params(&L, &in);
	newppm = ppm;
	return (struct walk){ newppm, ticks, prefill };
}

static double simulate(int start, struct walk w)
{
	double occ = start;
	for (int t = 0; t < w.ticks; t++) occ += -w.ppm * 1e-6;
	return occ;
}

int main(void)
{
	struct walk w;

	w = retarget(64, 150);                 /* 64 -> 1200 slots: a large grow */
	CHK(w.new_pf == 1200);
	CHK(w.ppm < 0.0);
	CHK(fabs(w.ppm) <= 2000.0 + 1e-6);
	CHK(fabs(simulate(64, w) - 1200.0) < 2.0);

	w = retarget(1200, 8);                 /* 1200 -> 64 slots: a large shrink */
	CHK(w.new_pf == 64);
	CHK(w.ppm > 0.0);
	CHK(fabs(w.ppm) <= 2000.0 + 1e-6);
	CHK(fabs(simulate(1200, w) - 64.0) < 2.0);

	w = retarget(100, 13);                 /* 100 -> 104 slots: a nudge, inside the cap */
	CHK(w.new_pf == 104);
	CHK(fabs(w.ppm) < 2000.0);
	CHK(fabs(simulate(100, w) - 104.0) < 1.0);

	w = retarget(200, 25);                 /* 200 -> 200 slots: no walk */
	CHK(w.new_pf == 200);
	CHK(w.ppm == 0.0);
	CHK(w.ticks == 0);

	printf("test_retarget: %d failures\n", fails);
	return fails ? 1 : 0;
}
