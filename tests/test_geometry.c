// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* frame_channels() — the frame geometry the inject paths run on.
 *
 * Four things are pinned here, and the second one is a correction to the record.
 *
 * 1. Every legal REAC audio frame length maps to its channel width, clean or
 *    with the OHRCA +2 CRC trailer an M-5000/M-480 fabric appends: an S-0808's
 *    340/342, an S-1608's 628/630, an S-4000's 1204/1206, the master's 1492/1494
 *    downstream broadcast.
 *
 * 2. The OHRCA trailer never mis-counted channels under the expression this
 *    replaced. `(len - 50) / 36` was reported as skipping the +2 strip and so
 *    computing the wrong width on an OHRCA fabric. It does skip the strip, but
 *    it cannot change the answer: the clean length is 52 + nch*36, so the divide
 *    sees 36*nch + 2, and the trailered one 36*nch + 4 — both truncate to nch,
 *    and it would take a trailer of 34 bytes or more to shift the quotient. This
 *    test asserts the equality across the whole legal domain so the claim is not
 *    made a third time. The real defect in that expression is (4) below.
 *
 * 3. libreac's reac_upstream_channels() is the same arithmetic done properly,
 *    and reac-repacer still cannot call it: it rejects the 40-channel solution,
 *    which is correct for a box return (1492 B is the downstream broadcast, never
 *    an upstream) and wrong here, because inj_copy_rx reads exactly that
 *    downstream broadcast off the wired OUT port. Pinned so a future "just use
 *    libreac's" simplification fails loudly instead of silently disabling
 *    --inject-copy on every rig. What the repacer does take from libreac is the
 *    trailer rule itself (reac_frame_clean_len) and the geometry constants.
 *
 * 4. Lengths that are not legal REAC audio frames are rejected. The old
 *    expression returned a plausible count for them, and a wrong nch is a wrong
 *    braid stride: the case built below shows the last time-sample's write
 *    landing on the C2 EA end marker — a frame the desk then drops.
 */

#define main repacer_main
#include "../tools/reac_repacer.c"
#undef main

#include <reac/reac_upstream.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* the expression frame_channels() replaced, kept only so (2) can be asserted */
static int old_channels(int len) { return (len - 50) / 36; }

int main(void)
{
	/* 1 + 2: the whole legal domain, clean and OHRCA-trailered */
	for (int nch = 2; nch <= 40; nch += 2) {
		int clean = 52 + nch * 36;
		CHK(frame_channels(clean) == nch);
		CHK(frame_channels(clean + 2) == nch);
		CHK(old_channels(clean) == nch);        /* the trailer never moved the count */
		CHK(old_channels(clean + 2) == nch);
	}

	/* 1: the widths that exist on the rig, spelled out */
	CHK(frame_channels(340) == 8   && frame_channels(342) == 8);    /* S-0808  */
	CHK(frame_channels(628) == 16  && frame_channels(630) == 16);   /* S-1608  */
	CHK(frame_channels(1204) == 32 && frame_channels(1206) == 32);  /* S-4000  */
	CHK(frame_channels(1492) == 40 && frame_channels(1494) == 40);  /* master downstream */

	/* 3: why libreac's upstream oracle is not the one called here */
	CHK(reac_upstream_channels(1206) == 32);   /* it does strip the trailer */
	CHK(reac_upstream_channels(1492) == -1);   /* ...but 40 ch is not an upstream */
	CHK(reac_upstream_channels(1494) == -1);
	CHK(frame_channels(1492) == 40);           /* and the inject paths must accept it */

	/* 4: malformed lengths decline instead of producing a stride */
	CHK(frame_channels(160) == -1);            /* 52 + 3*36: odd width, braid needs pairs */
	CHK(frame_channels(200) == -1);            /* off the 36-byte grid */
	CHK(frame_channels(50) == -1);             /* shorter than the overhead */
	CHK(frame_channels(52) == -1);             /* header + marker, no audio */
	CHK(frame_channels(1528) == -1);           /* 52 + 41*36: past the 40-ch ceiling */
	CHK(frame_channels(-4) == -1);

	/* 4, made concrete: a 160-byte frame. old_channels() says 3 channels, so the
	 * braid stride is 3*3 = 9 bytes and slot 2's twelfth sample writes at region
	 * offset (2 & ~1)*3 + 11*9 + 3 = 108 — one past the 108-byte region, i.e. the
	 * first byte of the C2 EA end marker. frame_channels() declines the frame, so
	 * inj_sine leaves it alone. */
	{
		uint8_t f[256];
		memset(f, 0, sizeof f);
		f[12] = 0x88; f[13] = 0x19;
		f[16] = 0x00; f[17] = 0x00;
		f[158] = 0xC2; f[159] = 0xEA;
		int bad_nch = old_channels(160);
		CHK(bad_nch == 3);
		CHK((bad_nch & ~1) * 3 + 11 * (bad_nch * 3) + 3 == bad_nch * 36);  /* == region end */

		g_inj_slot = 2; g_inj_amp = 7.5e6; g_inj_freq = 997.0; g_inj_ph = 0.1234;
		inj_sine(f, 160);
		CHK(f[50 + 158 - 50] == 0xC2 && f[159] == 0xEA);   /* end marker intact */
		int touched = 0;
		for (int i = 50; i < 158; i++)
			if (f[i]) touched = 1;
		CHK(!touched);                                     /* nothing written at all */
	}

	printf("test_geometry: %d failures\n", fails);
	return fails ? 1 : 0;
}
