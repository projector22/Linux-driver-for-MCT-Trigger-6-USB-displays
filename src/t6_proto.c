// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * MCT Trigger 6 protocol wire formats; see t6_proto.h and docs/PROTOCOL.md.
 */
#include "t6_proto.h"

/* Offsets within the 0x70-byte display section header. */
#define SECTION_VID		0x0c
#define SECTION_PID		0x0e
#define SECTION_CAPS1		0x58
#define SECTION_CAPS2		0x5c

int t6_section_parse(const u8 *buf, size_t len, struct t6_section *s)
{
	if (len < T6_SECTION_LEN)
		return -EPROTO;
	s->vid = get_unaligned_le16(buf + SECTION_VID);
	s->pid = get_unaligned_le16(buf + SECTION_PID);
	/* Caps word: LinkInterfaces in bits 0-3, timing count in bits 8-15. */
	s->link[0] = buf[SECTION_CAPS1] & 0x0f;
	s->link[1] = buf[SECTION_CAPS2] & 0x0f;
	s->num_timings[0] = buf[SECTION_CAPS1 + 1];
	s->num_timings[1] = buf[SECTION_CAPS2 + 1];
	return 0;
}

void t6_timing_decode(const u8 raw[T6_TIMING_LEN], struct t6_timing *t)
{
	t->clock_khz = get_unaligned_le32(raw + 0x00);
	t->refresh = raw[0x04];
	t->htotal = get_unaligned_le16(raw + 0x06);
	t->hdisplay = get_unaligned_le16(raw + 0x08);
	t->hsync_start = get_unaligned_le16(raw + 0x0a);
	t->hsync_width = get_unaligned_le16(raw + 0x0c);
	t->vtotal = get_unaligned_le16(raw + 0x0e);
	t->vdisplay = get_unaligned_le16(raw + 0x10);
	t->vsync_start = get_unaligned_le16(raw + 0x12);
	t->vsync_width = get_unaligned_le16(raw + 0x14);
	t->pll_frac_num = get_unaligned_le16(raw + 0x16);
	t->pll_frac_den = get_unaligned_le16(raw + 0x18);
	t->pll_int = raw[0x1a];
	t->pll_output_div = raw[0x1b];
	t->hsync_positive = raw[0x1c] != 0;
	t->vsync_positive = raw[0x1d] != 0;
	t->reduced_blanking = raw[0x1e];
	t->flags = raw[0x1f];
}

void t6_timing_encode(const struct t6_timing *t, u8 raw[T6_TIMING_LEN])
{
	memset(raw, 0, T6_TIMING_LEN);
	put_unaligned_le32(t->clock_khz, raw + 0x00);
	raw[0x04] = t->refresh;
	put_unaligned_le16(t->htotal, raw + 0x06);
	put_unaligned_le16(t->hdisplay, raw + 0x08);
	put_unaligned_le16(t->hsync_start, raw + 0x0a);
	put_unaligned_le16(t->hsync_width, raw + 0x0c);
	put_unaligned_le16(t->vtotal, raw + 0x0e);
	put_unaligned_le16(t->vdisplay, raw + 0x10);
	put_unaligned_le16(t->vsync_start, raw + 0x12);
	put_unaligned_le16(t->vsync_width, raw + 0x14);
	put_unaligned_le16(t->pll_frac_num, raw + 0x16);
	put_unaligned_le16(t->pll_frac_den, raw + 0x18);
	raw[0x1a] = t->pll_int;
	raw[0x1b] = t->pll_output_div;
	raw[0x1c] = t->hsync_positive;
	raw[0x1d] = t->vsync_positive;
	raw[0x1e] = t->reduced_blanking;
	raw[0x1f] = t->flags;
}

/*
 * Reject table entries that would make a nonsensical DRM mode; a firmware
 * table is trusted only as far as it is self-consistent.
 */
bool t6_timing_sane(const struct t6_timing *t)
{
	return t->clock_khz >= 10000 && t->clock_khz <= 600000 &&
	       t->refresh && t->pll_frac_den &&
	       t->hdisplay >= 320 && t->vdisplay >= 200 &&
	       t->hsync_start >= t->hdisplay && t->hsync_width &&
	       (u32)t->hsync_start + t->hsync_width <= t->htotal &&
	       t->vsync_start >= t->vdisplay && t->vsync_width &&
	       (u32)t->vsync_start + t->vsync_width <= t->vtotal;
}

void t6_bulk_hdr_encode(u32 payload_len, u32 addr, u8 out[T6_BULK_HDR_LEN])
{
	memset(out, 0, T6_BULK_HDR_LEN);
	put_unaligned_le32(T6_SIGNATURE_DISPLAY, out + 0x00);
	put_unaligned_le32(payload_len, out + 0x04);	/* total length */
	put_unaligned_le32(addr, out + 0x08);		/* VRAM destination */
	put_unaligned_le32(payload_len, out + 0x0c);	/* this transfer */
	/* 0x10 offset 0, 0x14 "more fragments" 0: one unfragmented object. */
}

void t6_flip_encode(const struct t6_flip *f, u8 out[T6_VIDEO_HDR_LEN])
{
	memset(out, 0, T6_VIDEO_HDR_LEN);
	put_unaligned_le32(f->command, out + 0x00);
	put_unaligned_le32(f->payload_size, out + 0x04);
	put_unaligned_le32(f->fence, out + 0x08);
	put_unaligned_le32(f->target_format, out + 0x0c);
	put_unaligned_le16(f->y_pitch, out + 0x10);
	put_unaligned_le16(f->uv_pitch, out + 0x12);
	put_unaligned_le32(f->y_addr, out + 0x14);
	put_unaligned_le32(f->uv_addr, out + 0x18);
	put_unaligned_le32(f->v_addr, out + 0x1c);
	put_unaligned_le32(f->source_format, out + 0x20);
	out[0x2f] = f->flags;
}

/*
 * MCT's layouts (triggerdm main.c, "1080p/jpg" cases):
 *
 *  one head:   ring [0, ram-12M)        fb at ram-12M, ram-8M, ram-4M
 *  two heads, primary (the HDMI-capable head):
 *              ring [0, ram-30M)        fb at ram-30M, ram-26M, ram-22M
 *  two heads, other:
 *              ring [ram-18M, ram-12M)  fb at ram-12M, ram-8M, ram-4M
 */
int t6_vram_layout(unsigned int vram_mb, unsigned int nheads, bool primary,
		   struct t6_vram_layout *l)
{
	unsigned int top, i;

	if (vram_mb < T6_VRAM_MIN_MB || vram_mb > 4095 || !nheads ||
	    nheads > T6_MAX_HEADS)
		return -EINVAL;

	if (nheads == 1) {
		l->ring_base = 0;
		top = vram_mb;
	} else if (primary) {
		l->ring_base = 0;
		top = vram_mb - 18;
	} else {
		l->ring_base = (vram_mb - 18) * SZ_1M;
		top = vram_mb;
	}

	for (i = 0; i < T6_FB_SLOTS; i++)
		l->fb[i] = (top - 12 + 4 * i) * SZ_1M;
	l->ring_end = l->fb[0];

	/* The ring must hold at least one maximum-size object. */
	if (l->ring_end - l->ring_base < T6_RING_MAX_OBJECT)
		return -EINVAL;
	return 0;
}

bool t6_mode_fits_vram(unsigned int width, unsigned int height)
{
	u64 nv12 = (u64)t6_nv12_uv_offset(width, height) +
		   (u64)t6_nv12_pitch(width) * ALIGN(height, 32) / 2;

	return width && height && nv12 <= T6_FB_SLOT_SIZE;
}

int t6_raw_layout(struct t6_vram_layout *l, unsigned int width,
		  unsigned int height, struct t6_raw_layout *raw)
{
	u64 bytes = (u64)t6_raw_pitch(width) * height;
	u32 slot, top;
	unsigned int i;

	if (!width || !height ||
	    T6_VIDEO_HDR_LEN + bytes > T6_RAW_MAX_OBJECT)
		return -ENOSPC;
	slot = ALIGN(T6_RAW_FB_OFFSET + (u32)bytes, SZ_1M);
	if (l->ring_end - l->ring_base < (u64)T6_FB_SLOTS * slot +
					 T6_RAW_MIN_RING)
		return -ENOSPC;

	top = l->ring_end - T6_FB_SLOTS * slot;
	for (i = 0; i < T6_FB_SLOTS; i++)
		raw->fb[i] = top + i * slot + T6_RAW_FB_OFFSET;
	l->ring_end = top;
	return 0;
}

u64 t6_raw_cost_update(u64 avg_ns, u64 frame_ns)
{
	if (!avg_ns)
		return frame_ns;
	return (avg_ns * (T6_RAW_COST_WEIGHT - 1) + frame_ns) /
	       T6_RAW_COST_WEIGHT;
}

bool t6_raw_motion_next(bool cur, u64 avg_ns, unsigned int min_fps)
{
	u64 period;

	if (!min_fps)
		return false;
	if (!avg_ns)
		return true;
	period = 1000000000U / min_fps;
	if (cur)
		return avg_ns <= period;
	return avg_ns * 8 < period * 7;
}

void t6_stream_reset(const struct t6_vram_layout *l, struct t6_stream_pos *s)
{
	s->ring_next = l->ring_base;
	s->fb_index = 0;
	s->frames = 0;
}

/*
 * Objects are placed at 1, 2 or 3 MiB granularity and the ring wraps when the
 * next object would cross into the framebuffers.  The JPEG engine is told to
 * reset on a wrap and for the first frames after a mode set, exactly as
 * MCT's driver does.
 */
int t6_stream_place(const struct t6_vram_layout *l, struct t6_stream_pos *s,
		    u32 len, u32 *ring_addr, u32 *fb_addr, bool *reset)
{
	u32 step;

	if (!len || len > T6_RING_MAX_OBJECT)
		return -E2BIG;
	step = ALIGN(len, SZ_1M);

	*reset = false;
	if (s->ring_next + step > l->ring_end) {
		s->ring_next = l->ring_base;
		*reset = true;
	}
	*ring_addr = s->ring_next;
	s->ring_next += step;

	s->fb_index = (s->fb_index + 1) % T6_FB_SLOTS;
	*fb_addr = l->fb[s->fb_index];

	if (s->frames < 10)
		*reset = true;
	s->frames++;
	return 0;
}

int t6_int_parse(const u8 *buf, size_t len, struct t6_int_event *ev)
{
	if (len < 0x14)
		return -EPROTO;
	ev->func_mask = buf[0x00];
	ev->fence = get_unaligned_le32(buf + 0x0c);
	ev->display_event = buf[0x13];
	return 0;
}

void t6_cursor_hdr_encode(unsigned int width, unsigned int height,
			  u8 out[T6_CURSOR_HDR_LEN])
{
	put_unaligned_le16(T6_CURSOR_TYPE_ARGB, out + 0);
	put_unaligned_le16(width, out + 2);
	put_unaligned_le16(height, out + 4);
	put_unaligned_le16(width * 4, out + 6);	/* pitch */
}

/*
 * Negative cursor coordinates (cursor partly off the left/top edge) are
 * sent as the one's complement of the magnitude, as MCT's driver does.
 */
u16 t6_cursor_coord(int v)
{
	return v < 0 ? (u16)~(u16)(-v) : (u16)v;
}
