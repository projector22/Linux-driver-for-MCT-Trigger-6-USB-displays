/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * MCT Trigger 6 ("T6") USB display protocol: wire formats.
 *
 * Everything here is pure data manipulation (no USB or DRM calls) so it can
 * be unit tested on the host; see tests/host/test_proto.c.  docs/PROTOCOL.md
 * describes the protocol in prose and lists the sources each fact comes from:
 * MCT's GPL "triggerdm" reference driver, USB captures of the Windows driver,
 * and cyrozap's reverse-engineering notes.
 *
 * All multi-byte fields are little-endian.
 */
#ifndef T6_PROTO_H
#define T6_PROTO_H

#include "t6_types.h"

#define T6_USB_VID			0x0711

/*
 * Endpoints of the single vendor-specific interface on real chips (the
 * driver finds them by type, so these are documentation).
 */
#define T6_EP_BULK_OUT			0x02
#define T6_EP_INT_IN			0x83

/* Vendor control requests (bmRequestType 0x40 OUT / 0xC0 IN, device). */
#define T6_REQ_SET_MONITOR_CTRL		0x03	/* wValue=head wIndex=0 off/1 on */
#define T6_REQ_SET_CURSOR1_POS		0x04	/* wValue=x wIndex=y */
#define T6_REQ_SET_CURSOR1_STATE	0x05	/* wValue=index wIndex=0/1 */
#define T6_REQ_SET_CURSOR2_POS		0x06
#define T6_REQ_SET_CURSOR2_STATE	0x07
#define T6_REQ_SET_CURSOR1_SHAPE	0x10	/* wValue=index wIndex=byte offset */
#define T6_REQ_SET_CURSOR2_SHAPE	0x11
#define T6_REQ_SET_TIMING		0x12	/* wValue=head, 32-byte timing */
#define T6_REQ_CANCEL_BULK_OUT		0x14	/* resync after a failed bulk write */
#define T6_REQ_SET_SOFTWARE_READY	0x31	/* wValue=head */
#define T6_REQ_GET_EDID			0x80	/* wValue=byte offset wIndex=head */
#define T6_REQ_GET_TIMING_COUNT		0x84	/* wValue=head, u32 */
#define T6_REQ_GET_CONN_STATUS		0x87	/* wValue=head, u8 0/1 */
#define T6_REQ_GET_VRAM_SIZE		0x88	/* u8, MiB */
#define T6_REQ_GET_TIMING_TABLE		0x89	/* wValue=head wIndex=byte offset */
#define T6_REQ_GET_VERSION		0xb0	/* wIndex=field, u32 */
#define T6_REQ_QUERY_SECTION		0xb3	/* display section header */

/* T6_REQ_GET_VERSION fields. */
#define T6_VERSION_HW_PLATFORM		0	/* 0: "Lite", 1: "Super Lite" */
#define T6_VERSION_BOOT_CODE		1
#define T6_VERSION_IMAGE_CODE		2

#define T6_MAX_HEADS			2
#define T6_EDID_BLOCK			128
#define T6_EDID_MAX_BLOCKS		4	/* the reference driver reads <= 4 */
#define T6_TIMING_TABLE_CHUNK		512	/* largest 0x89 read seen on the wire */
#define T6_TIMING_MAX_ENTRIES		128	/* sanity bound; real tables hold ~36 */

/* --- Display section header (T6_REQ_QUERY_SECTION) --------------------- */

#define T6_SECTION_LEN			0x70

/* Per-head "LinkInterfaces" bits: which transmitter drives the connector. */
#define T6_LINK_DAC			0x1	/* analogue VGA */
#define T6_LINK_DVO			0x2	/* external HDMI transmitter */
#define T6_LINK_DVI			0x4
#define T6_LINK_LVDS			0x8	/* internal HDMI */

struct t6_section {
	u16 vid, pid;		/* of the virtual display function */
	u8 link[T6_MAX_HEADS];	/* T6_LINK_* per head, 0 = head absent */
	u8 num_timings[T6_MAX_HEADS];
};

int t6_section_parse(const u8 *buf, size_t len, struct t6_section *s);

/* --- Display timing (T6_REQ_SET_TIMING and the 0x89 table) ------------- */

#define T6_TIMING_LEN			32

/*
 * PLL fields cannot be derived from the timing alone without knowing the
 * chip's reference clock, so modes are always taken verbatim from the chip's
 * own table (as MCT's drivers do) and the PLL fields are passed through.
 */
struct t6_timing {
	u32 clock_khz;
	u8 refresh;		/* Hz */
	u16 htotal, hdisplay, hsync_start, hsync_width;
	u16 vtotal, vdisplay, vsync_start, vsync_width;
	u16 pll_frac_num;	/* "FNUM"/P */
	u16 pll_frac_den;	/* "FDEN"/Q, 1000 in every table seen */
	u8 pll_int;		/* "IDIV"/N */
	u8 pll_output_div;	/* "OutputSelect" */
	bool hsync_positive, vsync_positive;
	u8 reduced_blanking;
	u8 flags;
};

void t6_timing_decode(const u8 raw[T6_TIMING_LEN], struct t6_timing *t);
void t6_timing_encode(const struct t6_timing *t, u8 raw[T6_TIMING_LEN]);
bool t6_timing_sane(const struct t6_timing *t);

/* --- Bulk OUT framing --------------------------------------------------- */

/*
 * Every bulk write is a 32-byte header followed by a separate transfer with
 * the payload, which the chip DMAs to @addr in its video RAM.  Only the
 * display "signature" is ever generated: other signatures select audio, or
 * firmware flashing (5), which this driver must never send.
 */
#define T6_BULK_HDR_LEN			32
#define T6_SIGNATURE_DISPLAY		0

void t6_bulk_hdr_encode(u32 payload_len, u32 addr, u8 out[T6_BULK_HDR_LEN]);

/* 48-byte video command at the start of a display payload. */
#define T6_VIDEO_HDR_LEN		48
#define T6_VIDEO_CMD_FLIP_PRIMARY	3	/* head 0 */
#define T6_VIDEO_CMD_FLIP_SECONDARY	4	/* head 1 */
#define T6_FORMAT_NV12			6
#define T6_FORMAT_RGB24			9	/* 3 bytes per pixel, B,G,R */
#define T6_FORMAT_JPEG			13
#define T6_VIDEO_FLAG_RESET_JPEG	0x80

/* JPEG payloads are followed by this many zero bytes, as MCT's driver does. */
#define T6_JPEG_PAD			1024

struct t6_flip {
	u32 command;
	u32 payload_size;	/* bytes after the 48-byte header */
	u32 fence;		/* echoed by the "frame done" interrupt */
	u32 target_format;
	u16 y_pitch, uv_pitch;
	u32 y_addr, uv_addr, v_addr;
	u32 source_format;
	u8 flags;
};

void t6_flip_encode(const struct t6_flip *f, u8 out[T6_VIDEO_HDR_LEN]);

/* --- Video RAM layout for JPEG streaming -------------------------------- */

/*
 * JPEG files are written into a ring ("command area") and the chip decodes
 * each one into the next of three NV12 framebuffers, then scans that out.
 * The addresses follow MCT's reference driver for 1080p-class JPEG output.
 */
#define T6_FB_SLOTS			3
#define T6_FB_SLOT_SIZE			(4 * SZ_1M)
#define T6_RING_MAX_OBJECT		(3 * SZ_1M)
#define T6_VRAM_MIN_MB			32

struct t6_vram_layout {
	u32 ring_base;
	u32 ring_end;		/* first byte after the ring */
	u32 fb[T6_FB_SLOTS];
};

int t6_vram_layout(unsigned int vram_mb, unsigned int nheads, bool primary,
		   struct t6_vram_layout *l);

/* NV12 surface geometry the chip decodes a width x height JPEG into. */
static inline u32 t6_nv12_pitch(unsigned int width)
{
	return ALIGN(width, 32);
}

static inline u32 t6_nv12_uv_offset(unsigned int width, unsigned int height)
{
	return t6_nv12_pitch(width) * ALIGN(height, 32) + T6_JPEG_PAD;
}

bool t6_mode_fits_vram(unsigned int width, unsigned int height);

/* --- Raw (lossless) frames ---------------------------------------------- */

/*
 * A raw frame is one object: the 48-byte video header immediately followed by
 * the pixels, written so that the pixels land on the framebuffer address
 * (the Windows driver's layout).  The chip scans the RGB24 surface out
 * directly; nothing is decoded.  Three such framebuffers are carved off the
 * top of the head's JPEG ring, so both kinds of frame can be mixed.
 */
#define T6_RAW_FB_OFFSET		0x40	/* pixels, 64-byte aligned */
#define T6_RAW_MIN_RING			(8 * SZ_1M)	/* left for JPEG */
#define T6_RAW_MAX_OBJECT		(T6_VIDEO_HDR_LEN + 2048 * 1200 * 3)

struct t6_raw_layout {
	u32 fb[T6_FB_SLOTS];	/* pixel addresses; header at fb - 48 */
};

static inline u32 t6_raw_pitch(unsigned int width)
{
	return width * 3;
}

/**
 * t6_raw_layout() - make room for raw frames of @width x @height
 * @l: the head's JPEG layout; on success its ring is shortened
 * @raw: output, the three raw framebuffers
 * Return: 0, or -ENOSPC if the frames don't fit next to a usable ring.
 */
int t6_raw_layout(struct t6_vram_layout *l, unsigned int width,
		  unsigned int height, struct t6_raw_layout *raw);

/*
 * Lossless motion policy.  While the picture moves, raw frames are used for
 * as long as they can still be delivered at the configured minimum frame
 * rate; below it, motion goes out as JPEG and the still picture is refined
 * with a raw frame.  @avg_ns is a running average of what one raw frame
 * costs (conversion, fence wait and transfer), 0 if not yet measured.
 */
#define T6_RAW_COST_WEIGHT	4	/* average over ~4 frames */

/**
 * t6_raw_cost_update() - fold one raw frame's cost into the running average
 * Return: the new average; the first measurement is taken as is.
 */
u64 t6_raw_cost_update(u64 avg_ns, u64 frame_ns);

/**
 * t6_raw_motion_next() - decide whether moving content goes out raw
 * @cur: the current decision
 * @avg_ns: running raw frame cost, 0 if unknown (try raw)
 * @min_fps: lowest acceptable raw frame rate; 0 = never raw during motion
 *
 * Switching back to raw needs 1/8 headroom below the frame period, so a
 * cost close to the limit doesn't flip the decision on every frame.
 */
bool t6_raw_motion_next(bool cur, u64 avg_ns, unsigned int min_fps);

/* Ring cursor and framebuffer rotation for one head. */
struct t6_stream_pos {
	u32 ring_next;
	unsigned int fb_index;
	unsigned int frames;	/* since the last mode set */
};

void t6_stream_reset(const struct t6_vram_layout *l, struct t6_stream_pos *s);

/**
 * t6_stream_place() - choose where the next JPEG object goes
 * @len: total object length (video header + JPEG + padding)
 * @ring_addr: output, bulk destination address
 * @fb_addr: output, NV12 framebuffer the chip decodes into
 * @reset: output, whether to set T6_VIDEO_FLAG_RESET_JPEG
 *
 * Return: 0, or -E2BIG if @len cannot fit in the ring.
 */
int t6_stream_place(const struct t6_vram_layout *l, struct t6_stream_pos *s,
		    u32 len, u32 *ring_addr, u32 *fb_addr, bool *reset);

/* --- Interrupt IN packets ----------------------------------------------- */

#define T6_INT_PKT_LEN			64
#define T6_INT_FUNC_DISPLAY		0x04
#define T6_INT_DISP_HOTPLUG0		0x01
#define T6_INT_DISP_HOTPLUG1		0x02
#define T6_INT_DISP_FRAME_DONE		0x04
#define T6_INT_DISP_JPEG_ERROR		0x08

struct t6_int_event {
	u8 func_mask;
	u8 display_event;	/* T6_INT_DISP_* */
	u32 fence;		/* for FRAME_DONE */
};

int t6_int_parse(const u8 *buf, size_t len, struct t6_int_event *ev);

/* --- Hardware cursor ---------------------------------------------------- */

#define T6_CURSOR_HDR_LEN		8
#define T6_CURSOR_TYPE_ARGB		1
#define T6_CURSOR_MAX_SIZE		128
#define T6_CURSOR_CHUNK			4096	/* largest control write MCT uses */

void t6_cursor_hdr_encode(unsigned int width, unsigned int height,
			  u8 out[T6_CURSOR_HDR_LEN]);
u16 t6_cursor_coord(int v);

#endif /* T6_PROTO_H */
