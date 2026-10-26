// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* repace_wire -- the frame readers and the emit-path rewrites of reac-repacer
 * (include/repace_wire.h). The geometry is libreac's: the EtherType, the frame
 * law 52 + nch * 36, the FCS-residue rule (reac_frame_clean_len) and the braid
 * byte map (reac_braid_pos), so the inject paths agree with the rest of the
 * FreeREAC tree byte for byte. */
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>

#include "repace_wire.h"

/* the EtherType at bytes 12-13 */
static int is_reac(const uint8_t *f)
{
	return f[12] == 0x88 && f[13] == 0x19;
}

/* an 802.1Q tag at bytes 12-13 with the REAC EtherType behind it at 16-17 */
static int is_tagged_reac(const uint8_t *f)
{
	return f[12] == 0x81 && f[13] == 0x00 && f[16] == 0x88 && f[17] == 0x19;
}

/* frame type 00 00 */
static int is_audio(const uint8_t *f)
{
	return f[16] == 0x00 && f[17] == 0x00;
}

int rp_fwd_class(const uint8_t *f, size_t n, int bcast_only)
{
	if (n < 18)
		return RP_FWD_DROP;
	if (!is_reac(f))
		return RP_FWD_DROP;
	if (bcast_only && f[0] != 0xff)
		return RP_FWD_DROP;
	return is_audio(f) ? RP_FWD_AUDIO : RP_FWD_CTRL;
}

int rp_ret_accept(const uint8_t *f, size_t n)
{
	return n >= 14 && is_reac(f);
}

int rp_ds_accept(const uint8_t *f, size_t n)
{
	if (n < 18)
		return 0;
	return is_reac(f) || is_tagged_reac(f);
}

int rp_meter_counter(const uint8_t *f, size_t n, uint16_t *ctr)
{
	size_t eo = (n > 13 && f[12] == 0x81 && f[13] == 0x00) ? 16 : 12;

	if (n < eo + 6 || n < 1000)
		return 0;
	if (!(f[eo] == 0x88 && f[eo + 1] == 0x19))
		return 0;
	*ctr = (uint16_t)(f[eo + 2] | (f[eo + 3] << 8));
	return 1;
}

unsigned rp_counter(const uint8_t *f)
{
	return (unsigned)(f[14] | (f[15] << 8));
}

int rp_frame_channels(long len)
{
	if (len < 0)
		return -1;
	size_t clean = reac_frame_clean_len((size_t)len);
	if (clean < REAC_UPSTREAM_OVERHEAD ||
	    (clean - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH != 0)
		return -1;
	int nch = (int)((clean - REAC_UPSTREAM_OVERHEAD) / REAC_UPSTREAM_BYTES_PER_CH);
	if (nch < 2 || nch > REAC_MAX_CHANNELS || (nch & 1))
		return -1;
	return nch;
}

size_t rp_copy_untag(uint8_t *f, size_t n)
{
	if (n < 50)
		return 0;
	if (is_tagged_reac(f)) {
		memmove(f + 12, f + 16, n - 16);
		n -= 4;
	}
	if (!reac_frame_is_reac(f, n) || !is_audio(f))
		return 0;
	return n;
}

int rp_slot_width(const uint8_t *f, size_t n, int slot)
{
	if (n < 50 || !is_audio(f))
		return -1;
	int nch = rp_frame_channels((long)n);
	if (nch < 0 || slot < 0 || slot >= nch)
		return -1;
	return nch;
}

int rp_slot_read(const uint8_t *f, size_t n, int slot, int32_t v[12])
{
	int nch = rp_slot_width(f, n, slot);
	if (nch < 0)
		return 0;
	const uint8_t *a = f + REAC_AUDIO_OFFSET;
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, slot, nch, pos);                      /* lo, mid, hi */
		int32_t x = a[pos[0]] | (a[pos[1]] << 8) | (a[pos[2]] << 16);
		v[s] = (x & 0x800000) ? x - (1 << 24) : x;              /* sign-extend bit 23 */
	}
	return 1;
}

int rp_slot_write(uint8_t *f, size_t n, int slot, const int32_t v[12])
{
	int nch = rp_slot_width(f, n, slot);
	if (nch < 0)
		return 0;
	uint8_t *a = f + REAC_AUDIO_OFFSET;
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, slot, nch, pos);
		a[pos[0]] = (uint8_t)(v[s] & 0xff);                     /* lo  */
		a[pos[1]] = (uint8_t)((v[s] >> 8) & 0xff);              /* mid */
		a[pos[2]] = (uint8_t)((v[s] >> 16) & 0xff);             /* hi  */
	}
	return 1;
}

void rp_restamp(uint8_t *f, unsigned ctr)
{
	f[14] = (uint8_t)(ctr & 0xff);
	f[15] = (uint8_t)((ctr >> 8) & 0xff);
}

void rp_mute(uint8_t *f, size_t n)
{
	if (n > 18 && is_audio(f))
		memset(f + 18, 0, n - 18);
}
