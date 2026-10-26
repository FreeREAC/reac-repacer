// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* repace_wire -- what reac-repacer reads from a frame off the wire, and the bytes
 * it writes into a frame it emits. The relay is byte-blind: it moves REAC frames
 * without decoding them. These are the few places it looks inside one, each a
 * reader of the daemon's (tools/reac_repacer.c) or a rewrite of its emit path.
 *
 * The frame, as the daemon reads it (offsets from the destination MAC):
 *   bytes 12-13  EtherType, 0x8819 for REAC (big-endian, as on the wire)
 *   bytes 14-15  the frame counter, little-endian
 *   bytes 16-17  the frame type: 00 00 an audio frame, anything else control
 *   byte  50     the audio region: 12 time-samples of nch s24 channels in the
 *                channel-pair braid (<reac/reac_braid.h>), nch * 36 bytes
 *   then         the C2 EA end marker, and on some capture paths two bytes of
 *                FCS residue after it
 * A VLAN-tagged frame carries 0x8100 at bytes 12-13 and the EtherType at 16-17.
 *
 * Every function here is pure: no socket, clock, thread or allocation. */
#ifndef REPACE_WIRE_H
#define REPACE_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* What the forward reader (tunnel -> ring) does with one frame: drop it, or queue
 * it as an audio frame or as a control frame. It takes a REAC frame of 18 bytes or
 * more; with bcast_only, only one whose destination MAC starts with 0xff (the
 * master's broadcast). A control frame is one whose type is not 00 00. */
enum rp_fwd { RP_FWD_DROP = 0, RP_FWD_AUDIO = 1, RP_FWD_CTRL = 2 };
int rp_fwd_class(const uint8_t *f, size_t n, int bcast_only);

/* The return relay (stagebox -> tunnel) passes every REAC frame of 14 bytes or
 * more, handshakes included: 1 passes it, 0 drops it. */
int rp_ret_accept(const uint8_t *f, size_t n);

/* The downstream listener (--pace-by-downstream) timestamps every REAC frame of 18
 * bytes or more, untagged or VLAN-tagged: 1 for one it timestamps. */
int rp_ds_accept(const uint8_t *f, size_t n);

/* The clock meter (--clock-source local) counts the frame counter of each REAC
 * frame of 1000 bytes or more, untagged or VLAN-tagged; the counter sits two bytes
 * after the EtherType. 1 and *ctr for a frame it counts, 0 for one it skips. */
int rp_meter_counter(const uint8_t *f, size_t n, uint16_t *ctr);

/* The frame counter of a queued frame (bytes 14-15), which the pacer seeds its
 * own output counter from. The frame holds at least 16 bytes. */
unsigned rp_counter(const uint8_t *f);

/* The channel width of a REAC audio frame of len bytes: the one even nch in
 * 2..40 with len = 52 + nch * 36, or that plus the two bytes of FCS residue; -1
 * for any other length. */
int rp_frame_channels(long len);

/* The inject-copy reader: the downstream frame it reads one channel of. A frame
 * of 50 bytes or more; a VLAN-tagged REAC frame is untagged in place first (its
 * four tag bytes removed, so the EtherType lands at 12-13). Returns the frame's
 * length after that, when it is a REAC audio frame; 0 for one the reader skips. */
size_t rp_copy_untag(uint8_t *f, size_t n);

/* Whether channel `slot` of an n-byte frame is one an inject path reads or
 * writes: an audio frame (type 00 00) of 50 bytes or more whose width
 * (rp_frame_channels) holds the slot. The width, or -1. */
int rp_slot_width(const uint8_t *f, size_t n, int slot);

/* The 12 s24 samples of channel `slot`, sign-extended, in time order: 1, or 0
 * (and v untouched) when rp_slot_width declines the frame. */
int rp_slot_read(const uint8_t *f, size_t n, int slot, int32_t v[12]);

/* Writes the low 24 bits of the 12 samples into channel `slot`, and no other
 * byte: 1, or 0 (and the frame untouched) when rp_slot_width declines it. */
int rp_slot_write(uint8_t *f, size_t n, int slot, const int32_t v[12]);

/* The emit path's own output counter, written to bytes 14-15 little-endian (its
 * low 16 bits). No other byte changes. */
void rp_restamp(uint8_t *f, unsigned ctr);

/* The rate-change mute: an audio frame of more than 18 bytes has every byte from
 * 18 to its end zeroed; any other frame keeps every byte. */
void rp_mute(uint8_t *f, size_t n);

#endif /* REPACE_WIRE_H */
