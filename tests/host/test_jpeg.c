// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Host tests for src/t6_jpeg.c.
 *
 * The encoder source is #included so the tests can reach its static helpers
 * and build a naive single-pass reference encoder from them.  That gives two
 * kinds of check:
 *
 *  1. Structural: assembling a frame from cached rows must produce exactly
 *     the bytes a straightforward one-pass encoder produces, including after
 *     partial (incremental) re-encodes.
 *  2. Semantic: every image is written to $T6_TEST_OUT together with its
 *     source pixels, and tests/host/check_jpeg.py decodes the JPEGs with an
 *     independent decoder (libjpeg via Pillow) and checks fidelity.
 *
 * Usage: test_jpeg <output-dir>     (exit status 0 on success)
 *        test_jpeg --bench          (encoder throughput, not a test)
 */
#include "../../src/t6_jpeg.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

/* --- Test images (XRGB8888, little-endian B,G,R,X) ---------------------- */

struct image {
	unsigned int w, h, pitch;
	u8 *px;
};

static struct image image_new(unsigned int w, unsigned int h)
{
	/* Pad the pitch so pitch != width*4 is exercised. */
	struct image img = { w, h, w * 4 + 64, NULL };

	img.px = calloc(img.pitch, h);
	if (!img.px)
		abort();
	return img;
}

static void set_px(struct image *img, unsigned int x, unsigned int y,
		   u8 r, u8 g, u8 b)
{
	u8 *p = img->px + (size_t)y * img->pitch + x * 4;

	p[0] = b;
	p[1] = g;
	p[2] = r;
	p[3] = 0xa5;	/* X byte must be ignored */
}

static u32 rng_state = 12345;

static u32 rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

enum pattern { PAT_GRADIENT, PAT_NOISE, PAT_DESKTOP, PAT_SOLID, PAT_COUNT };

static const char *const pattern_name[PAT_COUNT] = {
	"gradient", "noise", "desktop", "solid",
};

static void fill(struct image *img, enum pattern pat)
{
	unsigned int x, y;

	for (y = 0; y < img->h; y++) {
		for (x = 0; x < img->w; x++) {
			switch (pat) {
			case PAT_GRADIENT:
				set_px(img, x, y, x * 255 / (img->w ? img->w : 1),
				       y * 255 / (img->h ? img->h : 1),
				       (x + y) & 0xff);
				break;
			case PAT_NOISE: {
				u32 v = rng();

				set_px(img, x, y, v, v >> 8, v >> 16);
				break;
			}
			case PAT_DESKTOP:
				/* Flat panels, a title bar and "text" strokes. */
				if (y < 24)
					set_px(img, x, y, 40, 44, 52);
				else if ((x / 7 + y / 11) % 5 == 0 && (y % 11) < 8 &&
					 (x % 7) < 2)
					set_px(img, x, y, 20, 20, 20);
				else
					set_px(img, x, y, 236, 236, 240);
				break;
			case PAT_SOLID:
			default:
				set_px(img, x, y, 255, 128, 0);
				break;
			}
		}
	}
}

/* --- Encoding helpers --------------------------------------------------- */

struct rowset {
	unsigned int n;
	struct t6_jpeg_row *rows;
	size_t tail_cap;
};

static struct rowset rows_new(unsigned int w, unsigned int h)
{
	struct rowset rs;
	unsigned int r;

	rs.n = t6_jpeg_mcu_rows(h);
	rs.tail_cap = t6_jpeg_row_tail_max(w);
	rs.rows = calloc(rs.n, sizeof(*rs.rows));
	if (!rs.rows)
		abort();
	for (r = 0; r < rs.n; r++) {
		rs.rows[r].tail = malloc(rs.tail_cap);
		if (!rs.rows[r].tail)
			abort();
	}
	return rs;
}

static void rows_free(struct rowset *rs)
{
	unsigned int r;

	for (r = 0; r < rs->n; r++)
		free(rs->rows[r].tail);
	free(rs->rows);
}

static void encode_rows(const struct t6_jpeg_enc *enc, const struct image *img,
			struct rowset *rs, unsigned int first, unsigned int last)
{
	unsigned int r;

	for (r = first; r <= last && r < rs->n; r++) {
		int ret = t6_jpeg_encode_row(enc, img->px, img->pitch, img->w,
					     img->h, r, NULL, &rs->rows[r],
					     rs->tail_cap);

		CHECK(ret == 0, "encode_row %u failed: %d", r, ret);
	}
}

/* The obvious one-pass encoder, used as the reference. */
static long encode_reference(const struct t6_jpeg_enc *enc,
			     const struct image *img, u8 *out, size_t cap)
{
	unsigned int cols = t6_jpeg_mcu_cols(img->w);
	unsigned int rows = t6_jpeg_mcu_rows(img->h), mx, my;
	s16 mcu[T6_JPEG_BLOCKS_PER_MCU][64];
	s16 pred[3] = { 0, 0, 0 };
	struct bitwriter w;
	size_t hdr = t6_jpeg_write_header(enc, img->w, img->h, out);

	bw_init(&w, out + hdr, cap - hdr - 2, true);
	for (my = 0; my < rows; my++) {
		for (mx = 0; mx < cols; mx++) {
			transform_mcu(enc, img->px, img->pitch, img->w, img->h,
				      mx, my, NULL, mcu);
			encode_mcu(enc, &w, mcu, pred);
		}
	}
	bw_flush(&w);
	if (w.overflow)
		return -ENOSPC;
	*w.p++ = 0xff;
	*w.p++ = 0xd9;
	return w.p - out;
}

static void write_file(const char *dir, const char *name, const void *data,
		       size_t len)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f || fwrite(data, 1, len, f) != len) {
		perror(path);
		abort();
	}
	fclose(f);
}

/* Write the source as packed RGB24 so the checker can compare. */
static void write_rgb(const char *dir, const char *name, const struct image *img)
{
	u8 *rgb = malloc((size_t)img->w * img->h * 3 + 1);
	unsigned int x, y;

	for (y = 0; y < img->h; y++) {
		for (x = 0; x < img->w; x++) {
			const u8 *p = img->px + (size_t)y * img->pitch + x * 4;
			u8 *o = rgb + ((size_t)y * img->w + x) * 3;

			o[0] = p[2];
			o[1] = p[1];
			o[2] = p[0];
		}
	}
	write_file(dir, name, rgb, (size_t)img->w * img->h * 3);
	free(rgb);
}

/* --- Tests -------------------------------------------------------------- */

static void test_tables(void)
{
	struct t6_jpeg_enc enc;
	unsigned int t, k;

	t6_jpeg_init(&enc, 50);
	{
		u8 hdr[T6_JPEG_HEADER_LEN + 16];

		CHECK(t6_jpeg_write_header(&enc, 1920, 1080, hdr) == T6_JPEG_HEADER_LEN,
		      "header length");
	}
	/* Quality 50 must reproduce the Annex K tables exactly. */
	for (t = 0; t < 2; t++)
		for (k = 0; k < 64; k++)
			CHECK(enc.dqt[t][k] == base_quant[t][zigzag_to_natural[k]],
			      "q50 table %u[%u] = %u", t, k, enc.dqt[t][k]);

	/* Known canonical codes from T.81 Table K.3/K.5. */
	CHECK(enc.dc_code[0][0] == 0x0 && enc.dc_size[0][0] == 2, "DC luma 0");
	CHECK(enc.dc_code[0][11] == 0x1fe && enc.dc_size[0][11] == 9, "DC luma 11");
	CHECK(enc.ac_code[0][AC_EOB] == 0xa && enc.ac_size[0][AC_EOB] == 4, "AC EOB");
	CHECK(enc.ac_code[0][AC_ZRL] == 0x7f9 && enc.ac_size[0][AC_ZRL] == 11, "AC ZRL");
	CHECK(enc.ac_code[1][AC_EOB] == 0x0 && enc.ac_size[1][AC_EOB] == 2, "AC chroma EOB");

	/* Quality is clamped and 100 gives all-ones tables. */
	t6_jpeg_init(&enc, 0);
	CHECK(enc.quality == 1, "quality clamp low");
	t6_jpeg_init(&enc, 1000);
	CHECK(enc.quality == 100, "quality clamp high");
	for (k = 0; k < 64; k++)
		CHECK(enc.dqt[0][k] == 1, "q100 luma");
}

static void test_dct_dc(void)
{
	struct t6_jpeg_enc enc;
	s32 block[64];
	s16 out[64];
	unsigned int k;

	/* A flat block has only a DC term: 8 * mean, quantised. */
	t6_jpeg_init(&enc, 100);	/* divisor 8 everywhere */
	for (k = 0; k < 64; k++)
		block[k] = 100;
	transform_block(&enc, 0, block, out);
	CHECK(out[0] == 800, "flat DC = %d", out[0]);
	for (k = 1; k < 64; k++)
		CHECK(out[k] == 0, "flat AC[%u] = %d", k, out[k]);
}

/* Assembled output must equal the reference for many sizes and contents. */
static void test_matches_reference(const char *outdir)
{
	static const unsigned int sizes[][2] = {
		{ 1, 1 }, { 7, 5 }, { 16, 16 }, { 17, 33 }, { 33, 17 },
		{ 640, 480 }, { 1366, 768 }, { 1920, 1080 },
	};
	static const unsigned int qualities[] = { 50, 85, 95 };
	unsigned int s, p, q;

	for (s = 0; s < ARRAY_SIZE(sizes); s++) {
		for (p = 0; p < PAT_COUNT; p++) {
			for (q = 0; q < ARRAY_SIZE(qualities); q++) {
				struct image img = image_new(sizes[s][0], sizes[s][1]);
				size_t cap = t6_jpeg_frame_max(img.w, img.h);
				u8 *a = malloc(cap), *b = malloc(cap);
				struct rowset rs = rows_new(img.w, img.h);
				struct t6_jpeg_enc enc;
				long la, lb;
				char name[128];

				fill(&img, p);
				t6_jpeg_init(&enc, qualities[q]);
				encode_rows(&enc, &img, &rs, 0, rs.n - 1);
				la = t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, a, cap);
				lb = encode_reference(&enc, &img, b, cap);
				CHECK(la > 0 && la == lb && !memcmp(a, b, la),
				      "%ux%u %s q%u: assembled %ld vs reference %ld",
				      img.w, img.h, pattern_name[p], qualities[q], la, lb);

				snprintf(name, sizeof(name), "%s_%ux%u_q%u.jpg",
					 pattern_name[p], img.w, img.h, qualities[q]);
				if (la > 0)
					write_file(outdir, name, a, la);
				snprintf(name, sizeof(name), "%s_%ux%u_q%u.rgb",
					 pattern_name[p], img.w, img.h, qualities[q]);
				write_rgb(outdir, name, &img);

				rows_free(&rs);
				free(a);
				free(b);
				free(img.px);
			}
		}
	}
}

/* Re-encoding only the damaged rows must give the same frame. */
static void test_incremental(void)
{
	struct image img = image_new(1920, 1080);
	size_t cap = t6_jpeg_frame_max(img.w, img.h);
	u8 *a = malloc(cap), *b = malloc(cap);
	struct rowset rs = rows_new(img.w, img.h);
	struct t6_jpeg_enc enc;
	unsigned int iter;

	t6_jpeg_init(&enc, 85);
	fill(&img, PAT_DESKTOP);
	encode_rows(&enc, &img, &rs, 0, rs.n - 1);

	for (iter = 0; iter < 20; iter++) {
		/* Random damage rectangle, drawn with random content. */
		unsigned int x0 = rng() % img.w, y0 = rng() % img.h;
		unsigned int x1 = x0 + rng() % (img.w - x0);
		unsigned int y1 = y0 + rng() % (img.h - y0);
		unsigned int x, y;
		long la, lb;

		for (y = y0; y <= y1; y++)
			for (x = x0; x <= x1; x++)
				set_px(&img, x, y, rng(), x, y);

		encode_rows(&enc, &img, &rs, y0 / T6_JPEG_MCU, y1 / T6_JPEG_MCU);
		la = t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, a, cap);
		lb = encode_reference(&enc, &img, b, cap);
		CHECK(la > 0 && la == lb && !memcmp(a, b, la),
		      "incremental iteration %u differs (%ld vs %ld)", iter, la, lb);
	}
	rows_free(&rs);
	free(a);
	free(b);
	free(img.px);
}

/* Undersized buffers must fail cleanly, never overrun. */
static void test_overflow(void)
{
	struct image img = image_new(256, 64);
	struct rowset rs = rows_new(img.w, img.h);
	struct t6_jpeg_enc enc;
	u8 small_tail[64];
	struct t6_jpeg_row row;
	u8 *out;
	long len;
	size_t cap;

	t6_jpeg_init(&enc, 100);
	fill(&img, PAT_NOISE);

	row.tail = small_tail;
	CHECK(t6_jpeg_encode_row(&enc, img.px, img.pitch, img.w, img.h, 0,
				 NULL, &row, sizeof(small_tail)) == -ENOSPC,
	      "tiny tail buffer accepted");

	encode_rows(&enc, &img, &rs, 0, rs.n - 1);
	len = t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, NULL, 10);
	CHECK(len == -ENOSPC, "tiny frame buffer accepted");

	/* Exactly-sized buffer must work; one byte less must not. */
	cap = t6_jpeg_frame_max(img.w, img.h);
	out = malloc(cap);
	len = t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, out, cap);
	CHECK(len > 0, "full buffer failed");
	free(out);
	out = malloc(len);
	CHECK(t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, out, len) == len,
	      "exact buffer failed");
	CHECK(t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, out, len - 1) == -ENOSPC,
	      "short buffer accepted");
	free(out);
	rows_free(&rs);
	free(img.px);
}

/* S31.32 sign-magnitude, as in struct drm_color_ctm. */
static u64 drm_fixed(double v)
{
	u64 mag = (u64)((v < 0 ? -v : v) * 4294967296.0 + 0.5);

	return v < 0 ? mag | (1ull << 63) : mag;
}

/* Encoding with @cm must equal encoding pixels it was already applied to. */
static void check_color_encode(const struct t6_jpeg_enc *enc,
			       const struct image *img,
			       const struct t6_jpeg_color *cm, const char *what)
{
	struct image pre = image_new(img->w, img->h);
	struct rowset a = rows_new(img->w, img->h), b = rows_new(img->w, img->h);
	unsigned int x, y, r;

	for (y = 0; y < img->h; y++)
		for (x = 0; x < img->w; x++) {
			const u8 *s = img->px + (size_t)y * img->pitch + x * 4;
			s32 cr = s[2], cg = s[1], cb = s[0];

			apply_color(cm, &cr, &cg, &cb);
			set_px(&pre, x, y, cr, cg, cb);
		}
	for (r = 0; r < a.n; r++) {
		CHECK(t6_jpeg_encode_row(enc, img->px, img->pitch, img->w,
					 img->h, r, cm, &a.rows[r],
					 a.tail_cap) == 0, "%s: encode failed", what);
		CHECK(t6_jpeg_encode_row(enc, pre.px, pre.pitch, pre.w, pre.h,
					 r, NULL, &b.rows[r], b.tail_cap) == 0,
		      "%s: plain encode failed", what);
		CHECK(a.rows[r].tail_bits == b.rows[r].tail_bits &&
		      !memcmp(a.rows[r].tail, b.rows[r].tail,
			      DIV_ROUND_UP(a.rows[r].tail_bits, 8)) &&
		      !memcmp(a.rows[r].mcu0, b.rows[r].mcu0,
			      sizeof(a.rows[r].mcu0)),
		      "%s: row %u differs from pre-transformed pixels", what, r);
	}
	rows_free(&a);
	rows_free(&b);
	free(pre.px);
}

/* Colour management: the DRM CTM and GAMMA_LUT properties. */
static void test_color(void)
{
	/* vivid-screen's saturation 1.6 matrix, Rec. 709 luma weights. */
	static const double wt[3] = { 0.2126, 0.7152, 0.0722 };
	const double sat = 1.6;
	struct image img = image_new(100, 40);
	struct t6_jpeg_color cm = { 0 };
	struct t6_jpeg_enc enc;
	u16 lut[T6_JPEG_LUT_SIZE * 4];
	u64 drm[9];
	s32 m[9];
	unsigned int i;

	/* CTM conversion. */
	for (i = 0; i < 9; i++)
		drm[i] = drm_fixed(i % 4 == 0 ? 1.0 : 0.0);
	CHECK(!t6_jpeg_ctm_from_drm(drm, m), "identity CTM not detected");
	CHECK(m[0] == T6_JPEG_CTM_ONE && m[1] == 0, "identity CTM converted wrong");
	drm[1] = 1ull << 63;	/* negative zero */
	CHECK(!t6_jpeg_ctm_from_drm(drm, m), "-0 broke identity detection");
	drm[1] = drm_fixed(1e6);
	t6_jpeg_ctm_from_drm(drm, m);
	CHECK(m[1] == T6_JPEG_CTM_MAX, "huge coefficient not clamped: %d", m[1]);
	drm[1] = drm_fixed(-1e6);
	t6_jpeg_ctm_from_drm(drm, m);
	CHECK(m[1] == -T6_JPEG_CTM_MAX, "huge negative not clamped: %d", m[1]);

	for (i = 0; i < 9; i++)
		drm[i] = drm_fixed((1 - sat) * wt[i % 3] + (i % 4 == 0 ? sat : 0));
	CHECK(t6_jpeg_ctm_from_drm(drm, cm.ctm), "saturation CTM taken as identity");
	for (i = 0; i < 9; i++) {
		double want = ((1 - sat) * wt[i % 3] + (i % 4 == 0 ? sat : 0)) * 65536;

		CHECK(cm.ctm[i] - want < 1 && want - cm.ctm[i] < 1,
		      "CTM[%u] = %d, want %.1f", i, cm.ctm[i], want);
	}

	/* LUT conversion: the linear ramp X loads by default is the identity. */
	for (i = 0; i < T6_JPEG_LUT_SIZE; i++) {
		lut[i * 4 + 0] = lut[i * 4 + 1] = lut[i * 4 + 2] = i * 0x101;
		lut[i * 4 + 3] = 0xabcd;	/* reserved: ignored */
	}
	CHECK(!t6_jpeg_lut_from_drm(lut, cm.lut), "identity LUT not detected");
	/* A Night Color-like ramp: blue and green cut. */
	for (i = 0; i < T6_JPEG_LUT_SIZE; i++) {
		lut[i * 4 + 1] = i * 0x101 * 8 / 10;
		lut[i * 4 + 2] = i * 0x101 / 2;
	}
	CHECK(t6_jpeg_lut_from_drm(lut, cm.lut), "warm LUT taken as identity");
	CHECK(cm.lut[0][255] == 255 && cm.lut[1][255] == 204 &&
	      cm.lut[2][255] == 127 && cm.lut[2][0] == 0,
	      "LUT converted wrong: %u %u %u", cm.lut[0][255], cm.lut[1][255],
	      cm.lut[2][255]);

	/* Noise covers clamping at both ends. */
	t6_jpeg_init(&enc, 90);
	fill(&img, PAT_NOISE);
	cm.use_ctm = true;
	check_color_encode(&enc, &img, &cm, "CTM");
	cm.use_ctm = false;
	cm.use_lut = true;
	check_color_encode(&enc, &img, &cm, "LUT");
	cm.use_ctm = true;
	check_color_encode(&enc, &img, &cm, "CTM+LUT");

	/* Explicit identity settings change nothing. */
	for (i = 0; i < T6_JPEG_LUT_SIZE; i++)
		cm.lut[0][i] = cm.lut[1][i] = cm.lut[2][i] = i;
	for (i = 0; i < 9; i++)
		cm.ctm[i] = i % 4 == 0 ? T6_JPEG_CTM_ONE : 0;
	{
		struct rowset a = rows_new(img.w, img.h), b = rows_new(img.w, img.h);
		unsigned int r;

		for (r = 0; r < a.n; r++) {
			CHECK(t6_jpeg_encode_row(&enc, img.px, img.pitch, img.w,
						 img.h, r, &cm, &a.rows[r],
						 a.tail_cap) == 0,
			      "identity encode failed");
			encode_rows(&enc, &img, &b, r, r);
			CHECK(a.rows[r].tail_bits == b.rows[r].tail_bits &&
			      !memcmp(a.rows[r].tail, b.rows[r].tail,
				      DIV_ROUND_UP(a.rows[r].tail_bits, 8)),
			      "identity colour row %u differs", r);
		}
		rows_free(&a);
		rows_free(&b);
	}
	free(img.px);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int bench(void)
{
	static const enum pattern pats[] = { PAT_DESKTOP, PAT_GRADIENT, PAT_NOISE };
	struct image img = image_new(1920, 1080);
	size_t cap = t6_jpeg_frame_max(img.w, img.h);
	u8 *out = malloc(cap);
	struct rowset rs = rows_new(img.w, img.h);
	struct t6_jpeg_enc enc;
	unsigned int p, i, n = 20;

	t6_jpeg_init(&enc, 85);
	for (p = 0; p < ARRAY_SIZE(pats); p++) {
		double t0, t1, t2;
		long len = 0;

		fill(&img, pats[p]);
		t0 = now();
		for (i = 0; i < n; i++)
			encode_rows(&enc, &img, &rs, 0, rs.n - 1);
		t1 = now();
		for (i = 0; i < n; i++)
			len = t6_jpeg_assemble(&enc, img.w, img.h, rs.rows, out, cap);
		t2 = now();
		printf("1920x1080 %-8s q85: rows %.2f ms, assemble %.2f ms, %ld bytes\n",
		       pattern_name[pats[p]], (t1 - t0) * 1e3 / n,
		       (t2 - t1) * 1e3 / n, len);
	}
	rows_free(&rs);
	free(out);
	free(img.px);
	return 0;
}

static void test_bgr24(void)
{
	/* Two lines of three pixels with a padded pitch. */
	static const u8 src[2][16] = {
		{ 1, 2, 3, 0xff, 4, 5, 6, 0xff, 7, 8, 9, 0xff, 0xee, 0xee },
		{ 10, 11, 12, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0xee, 0xee },
	};
	struct t6_jpeg_color cm = { 0 };
	u8 out[18];
	unsigned int i;

	t6_color_bgr24(&src[0][0], 16, 3, 2, NULL, out);
	for (i = 0; i < 9; i++)
		CHECK(out[i] == i + 1, "bgr24 line 0 byte %u = %u", i, out[i]);
	CHECK(out[9] == 10 && out[12] == 255 && out[17] == 0, "bgr24 line 1");

	/* Inactive colour management is a plain copy. */
	t6_color_bgr24(&src[0][0], 16, 3, 2, &cm, out);
	CHECK(out[0] == 1 && out[8] == 9, "bgr24 identity colour");

	/* An inverting LUT, applied per channel, keeps B,G,R order. */
	cm.use_lut = true;
	for (i = 0; i < T6_JPEG_LUT_SIZE; i++)
		cm.lut[0][i] = cm.lut[1][i] = cm.lut[2][i] = 255 - i;
	cm.lut[2][1] = 100;	/* blue channel only */
	t6_color_bgr24(&src[0][0], 16, 3, 2, &cm, out);
	CHECK(out[0] == 100 && out[1] == 253 && out[2] == 252,
	      "bgr24 LUT %u %u %u", out[0], out[1], out[2]);
	CHECK(out[12] == 0 && out[15] == 255, "bgr24 LUT line 1");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--bench"))
		return bench();
	if (argc != 2) {
		fprintf(stderr, "usage: %s <output-dir> | --bench\n", argv[0]);
		return 2;
	}

	test_tables();
	test_dct_dc();
	test_matches_reference(argv[1]);
	test_incremental();
	test_overflow();
	test_color();
	test_bgr24();

	if (failures) {
		fprintf(stderr, "test_jpeg: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_jpeg: all checks passed\n");
	return 0;
}
