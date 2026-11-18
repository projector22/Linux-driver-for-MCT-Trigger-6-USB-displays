// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * KUnit tests for the hardware-independent parts of mct_t6.
 *
 * The host tests in tests/host cover the same code more exhaustively (with an
 * independent JPEG decoder); these make sure it also behaves in-kernel, built
 * with the kernel's compiler flags.  Run with:
 *
 *   tools/testing/kunit/kunit.py run --kunitconfig=<repo>/tests/kunit
 */
#include <kunit/test.h>
#include <linux/vmalloc.h>

/* Single module: pull in the code under test directly. */
#include "t6_jpeg.c"
#include "t6_proto.c"

static const u8 timing_1080p60[T6_TIMING_LEN] = {
	0x14, 0x44, 0x02, 0x00, 0x3c, 0x00, 0x98, 0x08,
	0x80, 0x07, 0xd8, 0x07, 0x2c, 0x00, 0x65, 0x04,
	0x38, 0x04, 0x3c, 0x04, 0x05, 0x00, 0xbb, 0x02,
	0xe8, 0x03, 0x1d, 0x01, 0x01, 0x01, 0x00, 0x00,
};

static void t6_test_timing_roundtrip(struct kunit *test)
{
	struct t6_timing t;
	u8 raw[T6_TIMING_LEN];

	t6_timing_decode(timing_1080p60, &t);
	KUNIT_EXPECT_EQ(test, t.clock_khz, 148500u);
	KUNIT_EXPECT_EQ(test, t.hdisplay, 1920);
	KUNIT_EXPECT_EQ(test, t.vdisplay, 1080);
	KUNIT_EXPECT_EQ(test, t.refresh, 60);
	KUNIT_EXPECT_TRUE(test, t6_timing_sane(&t));
	t6_timing_encode(&t, raw);
	KUNIT_EXPECT_EQ(test, memcmp(raw, timing_1080p60, sizeof(raw)), 0);
}

static void t6_test_vram_layout(struct kunit *test)
{
	struct t6_vram_layout l;

	KUNIT_ASSERT_EQ(test, t6_vram_layout(58, 1, true, &l), 0);
	KUNIT_EXPECT_EQ(test, l.ring_end, 46u * SZ_1M);
	KUNIT_EXPECT_EQ(test, l.fb[2], 54u * SZ_1M);
	KUNIT_ASSERT_EQ(test, t6_vram_layout(58, 2, false, &l), 0);
	KUNIT_EXPECT_EQ(test, l.ring_base, 40u * SZ_1M);
	KUNIT_EXPECT_EQ(test, t6_vram_layout(8, 1, true, &l), -EINVAL);
}

static void t6_test_stream_place(struct kunit *test)
{
	struct t6_vram_layout l;
	struct t6_stream_pos s;
	u32 ring, fb;
	bool reset;
	unsigned int i;

	t6_vram_layout(58, 2, false, &l);
	t6_stream_reset(&l, &s);
	for (i = 0; i < 50; i++) {
		u32 len = (i % 3 + 1) * 900 * 1024;

		KUNIT_ASSERT_EQ(test, t6_stream_place(&l, &s, len, &ring, &fb,
						      &reset), 0);
		KUNIT_EXPECT_GE(test, ring, l.ring_base);
		KUNIT_EXPECT_LE(test, ring + len, l.ring_end);
		if (i < 10)
			KUNIT_EXPECT_TRUE(test, reset);
	}
	KUNIT_EXPECT_EQ(test, t6_stream_place(&l, &s, 4 * SZ_1M, &ring, &fb,
					      &reset), -E2BIG);
}

static void t6_test_interrupt(struct kunit *test)
{
	u8 pkt[T6_INT_PKT_LEN] = { 0 };
	struct t6_int_event ev;

	pkt[0] = T6_INT_FUNC_DISPLAY;
	pkt[0x13] = T6_INT_DISP_FRAME_DONE;
	put_unaligned_le32(77, pkt + 0x0c);
	KUNIT_ASSERT_EQ(test, t6_int_parse(pkt, sizeof(pkt), &ev), 0);
	KUNIT_EXPECT_EQ(test, ev.display_event, T6_INT_DISP_FRAME_DONE);
	KUNIT_EXPECT_EQ(test, ev.fence, 77u);
}

/* Encode a synthetic frame from cached rows and check its structure. */
static void t6_test_jpeg_frame(struct kunit *test)
{
	const unsigned int w = 200, h = 120, pitch = w * 4;
	unsigned int nrows = t6_jpeg_mcu_rows(h), r, x, y;
	size_t cap = t6_jpeg_frame_max(w, h);
	struct t6_jpeg_row *rows;
	struct t6_jpeg_enc *enc;
	u8 *pixels, *out;
	long len;

	enc = kunit_kzalloc(test, sizeof(*enc), GFP_KERNEL);
	rows = kunit_kcalloc(test, nrows, sizeof(*rows), GFP_KERNEL);
	pixels = kunit_kzalloc(test, (size_t)pitch * h, GFP_KERNEL);
	out = kunit_kzalloc(test, cap, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, enc);
	KUNIT_ASSERT_NOT_NULL(test, rows);
	KUNIT_ASSERT_NOT_NULL(test, pixels);
	KUNIT_ASSERT_NOT_NULL(test, out);

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			u8 *p = pixels + y * pitch + x * 4;

			p[0] = x;
			p[1] = y * 2;
			p[2] = (x ^ y) & 0xff;
		}

	t6_jpeg_init(enc, 85);
	for (r = 0; r < nrows; r++) {
		rows[r].tail = kunit_kzalloc(test, t6_jpeg_row_tail_max(w),
					     GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, rows[r].tail);
		KUNIT_ASSERT_EQ(test, t6_jpeg_encode_row(enc, pixels, pitch, w,
							 h, r, NULL, &rows[r],
							 t6_jpeg_row_tail_max(w)), 0);
	}

	len = t6_jpeg_assemble(enc, w, h, rows, out, cap);
	KUNIT_ASSERT_GT(test, len, (long)T6_JPEG_HEADER_LEN);
	KUNIT_EXPECT_EQ(test, out[0], 0xff);
	KUNIT_EXPECT_EQ(test, out[1], 0xd8);		/* SOI */
	KUNIT_EXPECT_EQ(test, out[len - 2], 0xff);
	KUNIT_EXPECT_EQ(test, out[len - 1], 0xd9);	/* EOI */

	/* No marker may appear inside the entropy-coded data. */
	for (x = T6_JPEG_HEADER_LEN; x < len - 2; x++)
		if (out[x] == 0xff)
			KUNIT_EXPECT_EQ(test, out[x + 1], 0x00);

	KUNIT_EXPECT_EQ(test, t6_jpeg_assemble(enc, w, h, rows, out, 64),
			-ENOSPC);
}

static struct kunit_case t6_test_cases[] = {
	KUNIT_CASE(t6_test_timing_roundtrip),
	KUNIT_CASE(t6_test_vram_layout),
	KUNIT_CASE(t6_test_stream_place),
	KUNIT_CASE(t6_test_interrupt),
	KUNIT_CASE(t6_test_jpeg_frame),
	{}
};

static struct kunit_suite t6_test_suite = {
	.name = "mct_t6",
	.test_cases = t6_test_cases,
};

kunit_test_suite(t6_test_suite);

MODULE_DESCRIPTION("KUnit tests for the mct_t6 driver");
MODULE_LICENSE("GPL");
