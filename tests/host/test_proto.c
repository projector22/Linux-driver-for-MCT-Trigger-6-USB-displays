// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Host tests for src/t6_proto.c.  Golden byte vectors come from USB captures
 * of MCT's Windows driver (see docs/PROTOCOL.md for provenance).
 */
#include "../../src/t6_proto.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		if (!(cond)) {                                            \
			failures++;                                       \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                     \
			fputc('\n', stderr);                              \
		}                                                         \
	} while (0)

#define MiB(x) ((u32)(x) * SZ_1M)

/* 40 12 0000 0000: 1080p60 as sent by the Windows driver (JCD543). */
static const u8 timing_1080p60[T6_TIMING_LEN] = {
	0x14, 0x44, 0x02, 0x00, 0x3c, 0x00, 0x98, 0x08,
	0x80, 0x07, 0xd8, 0x07, 0x2c, 0x00, 0x65, 0x04,
	0x38, 0x04, 0x3c, 0x04, 0x05, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x01, 0x01, 0x01, 0x00, 0x00,
};

/* 40 12 0000 0000: 3840x2160@30 as sent by the Windows driver (JCD543). */
static const u8 timing_2160p30[T6_TIMING_LEN] = {
	0x28, 0x88, 0x04, 0x00, 0x1e, 0x00, 0x30, 0x11,
	0x00, 0x0f, 0xb0, 0x0f, 0x58, 0x00, 0xca, 0x08,
	0x70, 0x08, 0x78, 0x08, 0x0a, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x03, 0x01, 0x01, 0x00, 0x00,
};

/* Video header of a JPEG frame for head 1 (JUA365 capture, frame #166). */
static const u8 flip_jpeg_head1[T6_VIDEO_HDR_LEN] = {
	0x04, 0x00, 0x00, 0x00, 0xb0, 0x07, 0x01, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
	0x80, 0x07, 0x80, 0x07, 0x30, 0x04, 0x50, 0x02,
	0x30, 0xe4, 0x6f, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void test_timing(void)
{
	struct t6_timing t;
	u8 raw[T6_TIMING_LEN];

	t6_timing_decode(timing_1080p60, &t);
	CHECK(t.clock_khz == 148500 && t.refresh == 60, "1080p clock");
	CHECK(t.htotal == 2200 && t.hdisplay == 1920 && t.hsync_start == 2008 &&
	      t.hsync_width == 44, "1080p horizontal");
	CHECK(t.vtotal == 1125 && t.vdisplay == 1080 && t.vsync_start == 1084 &&
	      t.vsync_width == 5, "1080p vertical");
	CHECK(t.pll_frac_num == 699 && t.pll_frac_den == 1000 && t.pll_int == 29 &&
	      t.pll_output_div == 1, "1080p PLL");
	CHECK(t.hsync_positive && t.vsync_positive, "1080p polarity");
	CHECK(t6_timing_sane(&t), "1080p sane");
	t6_timing_encode(&t, raw);
	CHECK(!memcmp(raw, timing_1080p60, sizeof(raw)), "1080p round trip");

	t6_timing_decode(timing_2160p30, &t);
	CHECK(t.clock_khz == 297000 && t.refresh == 30 && t.hdisplay == 3840 &&
	      t.vdisplay == 2160 && t.pll_output_div == 3, "2160p decode");
	t6_timing_encode(&t, raw);
	CHECK(!memcmp(raw, timing_2160p30, sizeof(raw)), "2160p round trip");

	/* Self-inconsistent entries are rejected. */
	t6_timing_decode(timing_1080p60, &t);
	t.hsync_start = 1900;
	CHECK(!t6_timing_sane(&t), "hsync inside active area accepted");
	t6_timing_decode(timing_1080p60, &t);
	t.vtotal = 1088;
	CHECK(!t6_timing_sane(&t), "vsync past vtotal accepted");
	t6_timing_decode(timing_1080p60, &t);
	t.pll_frac_den = 0;
	CHECK(!t6_timing_sane(&t), "zero PLL denominator accepted");
}

static void test_flip(void)
{
	struct t6_flip f = {
		.command = T6_VIDEO_CMD_FLIP_SECONDARY,
		.payload_size = 0x107b0,
		.fence = 1,
		.target_format = T6_FORMAT_NV12,
		.y_pitch = 1920,
		.uv_pitch = 1920,
		.y_addr = 0x02500430,
		.uv_addr = 0x026fe430,
		.source_format = T6_FORMAT_JPEG,
	};
	u8 out[T6_VIDEO_HDR_LEN];

	t6_flip_encode(&f, out);
	CHECK(!memcmp(out, flip_jpeg_head1, sizeof(out)), "flip header bytes");

	f.flags = T6_VIDEO_FLAG_RESET_JPEG;
	t6_flip_encode(&f, out);
	CHECK(out[0x2f] == 0x80, "reset flag position");
}

static void test_bulk_hdr(void)
{
	u8 out[T6_BULK_HDR_LEN];
	unsigned int i;

	memset(out, 0xee, sizeof(out));
	t6_bulk_hdr_encode(0x12345, 0x03000000, out);
	CHECK(get_unaligned_le32(out + 0) == T6_SIGNATURE_DISPLAY, "signature");
	CHECK(get_unaligned_le32(out + 4) == 0x12345, "payload length");
	CHECK(get_unaligned_le32(out + 8) == 0x03000000, "address");
	CHECK(get_unaligned_le32(out + 12) == 0x12345, "packet length");
	for (i = 16; i < T6_BULK_HDR_LEN; i++)
		CHECK(out[i] == 0, "reserved byte %u = %#x", i, out[i]);
}

static void test_vram_layout(void)
{
	struct t6_vram_layout l;

	/* 58 MiB parts, matching triggerdm's diagrams and the captures. */
	CHECK(!t6_vram_layout(58, 1, true, &l), "single head");
	CHECK(l.ring_base == 0 && l.ring_end == MiB(46), "single ring");
	CHECK(l.fb[0] == MiB(46) && l.fb[1] == MiB(50) && l.fb[2] == MiB(54),
	      "single fbs");

	CHECK(!t6_vram_layout(58, 2, true, &l), "dual primary");
	CHECK(l.ring_base == 0 && l.ring_end == MiB(28), "dual primary ring");
	CHECK(l.fb[0] == MiB(28) && l.fb[2] == MiB(36), "dual primary fbs");

	CHECK(!t6_vram_layout(58, 2, false, &l), "dual secondary");
	CHECK(l.ring_base == MiB(40) && l.ring_end == MiB(46), "dual secondary ring");
	CHECK(l.fb[0] == MiB(46) && l.fb[2] == MiB(54), "dual secondary fbs");

	/* Every slot must stay inside VRAM. */
	CHECK(l.fb[2] + T6_FB_SLOT_SIZE <= MiB(58), "top slot inside VRAM");

	CHECK(t6_vram_layout(16, 1, true, &l) == -EINVAL, "tiny VRAM accepted");
	CHECK(t6_vram_layout(58, 0, true, &l) == -EINVAL, "zero heads accepted");
	CHECK(t6_vram_layout(58, 3, true, &l) == -EINVAL, "three heads accepted");
}

static void test_raw_layout(void)
{
	struct t6_vram_layout l;
	struct t6_raw_layout raw;
	u32 bytes = 1920 * 1080 * 3;
	unsigned int i;

	/* Single head, 1080p: three 6 MiB raw buffers off the ring's top. */
	t6_vram_layout(58, 1, true, &l);
	CHECK(!t6_raw_layout(&l, 1920, 1080, &raw), "1080p raw fits");
	CHECK(l.ring_base == 0 && l.ring_end == MiB(28), "ring shortened");
	for (i = 0; i < T6_FB_SLOTS; i++) {
		CHECK(raw.fb[i] == MiB(28 + 6 * i) + T6_RAW_FB_OFFSET,
		      "raw fb %u at %#x", i, raw.fb[i]);
		CHECK(raw.fb[i] - T6_VIDEO_HDR_LEN >= l.ring_end,
		      "raw header %u overlaps the ring", i);
		CHECK(raw.fb[i] + bytes <= l.fb[0], "raw fb %u overlaps NV12", i);
		CHECK(raw.fb[i] % 64 == 0, "raw fb %u unaligned", i);
	}
	CHECK(raw.fb[1] - raw.fb[0] >= bytes + T6_VIDEO_HDR_LEN, "raw overlap");
	CHECK(l.ring_end - l.ring_base >= T6_RING_MAX_OBJECT, "ring too small");

	/* Dual head: the primary has room, the secondary's 6 MiB ring not. */
	t6_vram_layout(58, 2, true, &l);
	CHECK(!t6_raw_layout(&l, 1920, 1080, &raw), "dual primary raw");
	CHECK(l.ring_end - l.ring_base >= T6_RAW_MIN_RING, "dual ring kept");
	t6_vram_layout(58, 2, false, &l);
	CHECK(t6_raw_layout(&l, 1920, 1080, &raw) == -ENOSPC,
	      "dual secondary raw must not fit");
	CHECK(l.ring_end == MiB(46), "failed raw layout changed the ring");

	t6_vram_layout(58, 1, true, &l);
	CHECK(!t6_raw_layout(&l, 2048, 1152, &raw), "2048x1152 raw");
	t6_vram_layout(58, 1, true, &l);
	CHECK(t6_raw_layout(&l, 3840, 2160, &raw) == -ENOSPC, "4K raw");
	CHECK(t6_raw_layout(&l, 0, 1080, &raw) == -ENOSPC, "zero width");
}

static void test_raw_motion(void)
{
	const u64 ms = 1000000;

	/* The cost average: first sample as is, then weighted 3:1. */
	CHECK(t6_raw_cost_update(0, 36 * ms) == 36 * ms, "first sample");
	CHECK(t6_raw_cost_update(36 * ms, 40 * ms) == 37 * ms, "average");

	/* min_fps 0: motion is never raw. */
	CHECK(!t6_raw_motion_next(true, 1 * ms, 0), "min_fps 0");
	/* Unmeasured: try raw. */
	CHECK(t6_raw_motion_next(false, 0, 20), "unmeasured tries raw");

	/* 1080p on SuperSpeed (~36 ms) keeps raw at 20 fps (50 ms). */
	CHECK(t6_raw_motion_next(true, 36 * ms, 20), "36 ms at 20 fps");
	CHECK(t6_raw_motion_next(true, 50 * ms, 20), "exactly the period");
	/* Too slow (USB 2, two heads): fall back to JPEG for motion. */
	CHECK(!t6_raw_motion_next(true, 51 * ms, 20), "51 ms at 20 fps");
	CHECK(!t6_raw_motion_next(true, 180 * ms, 20), "USB 2 raw");

	/* Hysteresis: back to raw only below 7/8 of the period. */
	CHECK(!t6_raw_motion_next(false, 50 * ms, 20), "no flip at period");
	CHECK(!t6_raw_motion_next(false, 44 * ms, 20), "no flip at 44 ms");
	CHECK(t6_raw_motion_next(false, 43 * ms, 20), "flip at 43 ms");
}

static void test_mode_fits(void)
{
	CHECK(t6_mode_fits_vram(1920, 1080), "1080p");
	CHECK(t6_mode_fits_vram(1920, 1200), "1920x1200");
	CHECK(t6_mode_fits_vram(2048, 1152), "2048x1152");
	CHECK(!t6_mode_fits_vram(2560, 1440), "1440p must not fit a 4 MiB slot");
	CHECK(!t6_mode_fits_vram(3840, 2160), "4K must not fit a 4 MiB slot");
	CHECK(!t6_mode_fits_vram(0, 1080), "zero width");
	CHECK(t6_nv12_pitch(1366) == 1376, "pitch alignment");
	CHECK(t6_nv12_uv_offset(1920, 1080) == 1920 * 1088 + 1024, "uv offset");
}

static void test_stream(void)
{
	struct t6_vram_layout l;
	struct t6_stream_pos s;
	u32 ring, fb, prev_fb = 0;
	bool reset;
	unsigned int i, wraps = 0;

	t6_vram_layout(58, 2, false, &l);	/* 6 MiB ring */
	t6_stream_reset(&l, &s);

	CHECK(t6_stream_place(&l, &s, 0, &ring, &fb, &reset) == -E2BIG, "empty");
	CHECK(t6_stream_place(&l, &s, T6_RING_MAX_OBJECT + 1, &ring, &fb,
			      &reset) == -E2BIG, "oversized");

	for (i = 0; i < 100; i++) {
		u32 len = (i % 3 + 1) * 700 * 1024;	/* 0.7, 1.4, 2.1 MiB */
		int ret = t6_stream_place(&l, &s, len, &ring, &fb, &reset);

		CHECK(ret == 0, "place %u", i);
		CHECK(ring >= l.ring_base && ring + len <= l.ring_end,
		      "object %u at %#x+%#x escapes ring", i, ring, len);
		CHECK(ring % SZ_1M == 0, "object %u unaligned", i);
		CHECK(fb != prev_fb, "fb %u reused back-to-back", i);
		CHECK(fb == l.fb[0] || fb == l.fb[1] || fb == l.fb[2], "fb %u", i);
		if (i < 10)
			CHECK(reset, "frame %u must reset the JPEG engine", i);
		if (i >= 10 && ring == l.ring_base) {
			CHECK(reset, "wrap at frame %u without reset", i);
			wraps++;
		}
		if (i >= 10 && ring != l.ring_base)
			CHECK(!reset, "spurious reset at frame %u", i);
		prev_fb = fb;
	}
	CHECK(wraps > 10, "ring never wrapped (%u)", wraps);
}

static void test_interrupt(void)
{
	u8 pkt[T6_INT_PKT_LEN] = { 0 };
	struct t6_int_event ev;

	CHECK(t6_int_parse(pkt, 8, &ev) == -EPROTO, "short packet");

	/* Hot-plug on connector 1: type 4, dword 4 = 0x02000000. */
	put_unaligned_le32(0x04, pkt + 0);
	put_unaligned_le32(0x02000000, pkt + 0x10);
	CHECK(!t6_int_parse(pkt, sizeof(pkt), &ev), "parse");
	CHECK(ev.func_mask == T6_INT_FUNC_DISPLAY &&
	      ev.display_event == T6_INT_DISP_HOTPLUG1, "hotplug 1");

	/* Frame done: dword 4 = 0x04000000, dword 3 = fence. */
	put_unaligned_le32(0x04000000, pkt + 0x10);
	put_unaligned_le32(1234, pkt + 0x0c);
	CHECK(!t6_int_parse(pkt, sizeof(pkt), &ev), "parse");
	CHECK(ev.display_event == T6_INT_DISP_FRAME_DONE && ev.fence == 1234,
	      "frame done");
}

static void test_section(void)
{
	u8 buf[T6_SECTION_LEN] = { 0 };
	struct t6_section s;

	CHECK(t6_section_parse(buf, 16, &s) == -EPROTO, "short section");
	put_unaligned_le16(0x0711, buf + 0x0c);
	put_unaligned_le16(0x5621, buf + 0x0e);
	put_unaligned_le32(0x00102402, buf + 0x58);	/* HDMI, 36 timings */
	put_unaligned_le32(0x00101e01, buf + 0x5c);	/* VGA, 30 timings */
	CHECK(!t6_section_parse(buf, sizeof(buf), &s), "parse");
	CHECK(s.vid == 0x0711 && s.pid == 0x5621, "ids");
	CHECK(s.link[0] == T6_LINK_DVO && s.link[1] == T6_LINK_DAC, "links");
	CHECK(s.num_timings[0] == 36 && s.num_timings[1] == 30, "timing counts");
}

static void test_cursor(void)
{
	u8 hdr[T6_CURSOR_HDR_LEN];

	t6_cursor_hdr_encode(64, 64, hdr);
	CHECK(get_unaligned_le16(hdr + 0) == T6_CURSOR_TYPE_ARGB &&
	      get_unaligned_le16(hdr + 2) == 64 &&
	      get_unaligned_le16(hdr + 4) == 64 &&
	      get_unaligned_le16(hdr + 6) == 256, "cursor header");
	CHECK(t6_cursor_coord(0) == 0 && t6_cursor_coord(1919) == 1919, "coords");
	CHECK(t6_cursor_coord(-5) == 0xfffa, "negative coord");
}

int main(void)
{
	test_timing();
	test_flip();
	test_bulk_hdr();
	test_vram_layout();
	test_raw_layout();
	test_raw_motion();
	test_mode_fits();
	test_stream();
	test_interrupt();
	test_section();
	test_cursor();

	if (failures) {
		fprintf(stderr, "test_proto: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_proto: all checks passed\n");
	return 0;
}
