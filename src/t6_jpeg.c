// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Baseline JPEG encoder; see t6_jpeg.h for the design.
 *
 * The algorithms are the textbook ones from ITU-T T.81 (JPEG) and the IJG
 * reference implementation's integer "islow" forward DCT (Loeffler,
 * Ligtenberg and Moschytz, 1989).  Constant tables are the T.81 Annex K
 * examples, which every baseline decoder handles.
 */
#include "t6_jpeg.h"

/* --- Constant tables ---------------------------------------------------- */

/* Zigzag position -> natural (row-major) coefficient index. */
static const u8 zigzag_to_natural[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* T.81 Annex K.1 quantisation tables, natural order. */
static const u8 base_quant[2][64] = {
	{ /* luminance */
		16,  11,  10,  16,  24,  40,  51,  61,
		12,  12,  14,  19,  26,  58,  60,  55,
		14,  13,  16,  24,  40,  57,  69,  56,
		14,  17,  22,  29,  51,  87,  80,  62,
		18,  22,  37,  56,  68, 109, 103,  77,
		24,  35,  55,  64,  81, 104, 113,  92,
		49,  64,  78,  87, 103, 121, 120, 101,
		72,  92,  95,  98, 112, 100, 103,  99,
	},
	{ /* chrominance */
		17,  18,  24,  47,  99,  99,  99,  99,
		18,  21,  26,  66,  99,  99,  99,  99,
		24,  26,  56,  99,  99,  99,  99,  99,
		47,  66,  99,  99,  99,  99,  99,  99,
		99,  99,  99,  99,  99,  99,  99,  99,
		99,  99,  99,  99,  99,  99,  99,  99,
		99,  99,  99,  99,  99,  99,  99,  99,
		99,  99,  99,  99,  99,  99,  99,  99,
	},
};

/*
 * T.81 Annex K.3 Huffman tables: BITS[i] = number of codes of length i+1,
 * followed by the symbol values in order of increasing code length.
 */
static const u8 dc_bits[2][16] = {
	{ 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
};

static const u8 dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const u8 ac_bits[2][16] = {
	{ 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d },
	{ 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },
};

static const u8 ac_vals[2][162] = {
	{
		0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
		0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
		0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
		0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
		0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
		0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
		0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
		0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
		0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
		0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
		0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
		0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
		0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
		0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
		0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
		0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
		0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
		0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
		0xf9, 0xfa,
	},
	{
		0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
		0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
		0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
		0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
		0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
		0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
		0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
		0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
		0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
		0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
		0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
		0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
		0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
		0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
		0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
		0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
		0xf9, 0xfa,
	},
};

/* Run/size symbols with special meaning in AC coding. */
#define AC_EOB 0x00
#define AC_ZRL 0xf0

/* Quantised values are clamped to what baseline categories can express. */
#define DC_MAX 2047
#define AC_MAX 1023

/* --- Table construction ------------------------------------------------- */

/* T.81 Annex C: derive canonical code words from BITS/HUFFVAL. */
static void build_huffman(const u8 bits[16], const u8 *vals,
			  u16 *code_out, u8 *size_out)
{
	unsigned int len, i, k = 0;
	u16 code = 0;

	for (len = 1; len <= 16; len++) {
		for (i = 0; i < bits[len - 1]; i++, k++) {
			code_out[vals[k]] = code++;
			size_out[vals[k]] = len;
		}
		code <<= 1;
	}
}

void t6_jpeg_init(struct t6_jpeg_enc *enc, unsigned int quality)
{
	unsigned int t, k, scale;

	memset(enc, 0, sizeof(*enc));
	quality = clamp(quality, 1u, 100u);
	enc->quality = quality;

	/* IJG quality scaling, so "quality" means what users expect. */
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

	for (t = 0; t < 2; t++) {
		for (k = 0; k < 64; k++) {
			unsigned int q = base_quant[t][zigzag_to_natural[k]];
			unsigned int div;

			q = clamp((q * scale + 50) / 100, 1u, 255u);
			enc->dqt[t][k] = q;
			/* The forward DCT output is scaled up by 8. */
			div = q * 8;
			enc->recip[t][k] = ((1u << 20) + div - 1) / div;
			enc->bias[t][k] = div / 2;
		}
		build_huffman(dc_bits[t], dc_vals, enc->dc_code[t],
			      enc->dc_size[t]);
		build_huffman(ac_bits[t], ac_vals[t], enc->ac_code[t],
			      enc->ac_size[t]);
	}
}

/* --- Bit writer --------------------------------------------------------- */

/*
 * Accumulates Huffman codes MSB-first.  With @stuff set it inserts the 0x00
 * that must follow every 0xFF byte in entropy-coded data; row tails are
 * stored unstuffed and only stuffed once, during assembly.
 */
struct bitwriter {
	u8 *p;
	u8 *end;
	u64 acc;
	unsigned int nbits;	/* pending bits in the low end of acc, < 8 */
	bool stuff;
	bool overflow;
};

static void bw_init(struct bitwriter *w, u8 *buf, size_t cap, bool stuff)
{
	w->p = buf;
	w->end = buf + cap;
	w->acc = 0;
	w->nbits = 0;
	w->stuff = stuff;
	w->overflow = false;
}

static inline void bw_emit(struct bitwriter *w, u8 byte)
{
	bool stuff = w->stuff && byte == 0xff;

	if (unlikely(w->end - w->p < 1 + stuff)) {
		w->overflow = true;
		return;
	}
	*w->p++ = byte;
	if (stuff)
		*w->p++ = 0;
}

/* Append the low @len bits of @bits; @len <= 32. */
static inline void bw_put(struct bitwriter *w, u32 bits, unsigned int len)
{
	if (!len)
		return;
	w->acc = (w->acc << len) | (bits & (u32)((1ull << len) - 1));
	w->nbits += len;
	while (w->nbits >= 8) {
		w->nbits -= 8;
		bw_emit(w, w->acc >> w->nbits);
	}
}

/* Append @nbits bits stored MSB-first in @src. */
static void bw_put_stream(struct bitwriter *w, const u8 *src, u32 nbits)
{
	u32 whole = nbits / 8, rem = nbits % 8, i;

	for (i = 0; i < whole && !w->overflow; i++)
		bw_put(w, src[i], 8);
	if (rem)
		bw_put(w, src[whole] >> (8 - rem), rem);
}

/* Pad to a byte boundary with 1-bits, as T.81 F.1.2.3 requires. */
static void bw_flush(struct bitwriter *w)
{
	if (w->nbits)
		bw_put(w, 0x7f, 8 - w->nbits);
}

/* --- Colour conversion -------------------------------------------------- */

bool t6_jpeg_ctm_from_drm(const u64 drm[9], s32 out[9])
{
	bool identity = true;
	unsigned int i;

	for (i = 0; i < 9; i++) {
		u64 mag = drm[i] & ~(1ull << 63);
		s32 v;

		/* S31.32 -> 16.16, rounded; clamp before narrowing. */
		mag = (mag + (1u << 15)) >> 16;
		v = (s32)min_t(u64, mag, T6_JPEG_CTM_MAX);
		if (drm[i] >> 63)
			v = -v;
		out[i] = v;
		if (v != (i % 4 == 0 ? T6_JPEG_CTM_ONE : 0))
			identity = false;
	}
	return !identity;
}

/*
 * One output channel of the colour matrix.  Coefficients are capped at 8.0,
 * so the sum is at most 3 * 8 * 255 * 2^16 < 2^31.
 */
static inline s32 ctm_channel(const s32 m[3], s32 r, s32 g, s32 b)
{
	return clamp_t(s32, (m[0] * r + m[1] * g + m[2] * b + 32768) >> 16,
		       0, 255);
}

bool t6_jpeg_lut_from_drm(const u16 *drm, u8 out[3][T6_JPEG_LUT_SIZE])
{
	bool identity = true;
	unsigned int i, c;

	for (i = 0; i < T6_JPEG_LUT_SIZE; i++)
		for (c = 0; c < 3; c++) {
			/* 16 to 8 bits, rounded: 0xffff -> 255. */
			u8 v = ((u32)drm[i * 4 + c] * 255 + 32767) / 65535;

			out[c][i] = v;
			if (v != i)
				identity = false;
		}
	return !identity;
}

/* Apply colour management to one pixel in place: CTM, then gamma. */
static inline void apply_color(const struct t6_jpeg_color *cm,
			       s32 *r, s32 *g, s32 *b)
{
	if (cm->use_ctm) {
		const s32 *m = cm->ctm;
		s32 in_r = *r, in_g = *g, in_b = *b;

		*r = ctm_channel(m, in_r, in_g, in_b);
		*g = ctm_channel(m + 3, in_r, in_g, in_b);
		*b = ctm_channel(m + 6, in_r, in_g, in_b);
	}
	if (cm->use_lut) {
		*r = cm->lut[0][*r];
		*g = cm->lut[1][*g];
		*b = cm->lut[2][*b];
	}
}

void t6_color_bgr24(const u8 *src, unsigned int pitch, unsigned int width,
		    unsigned int lines, const struct t6_jpeg_color *color,
		    u8 *dst)
{
	unsigned int x, y;

	if (color && !color->use_ctm && !color->use_lut)
		color = NULL;

	for (y = 0; y < lines; y++) {
		const u8 *p = src + (size_t)y * pitch;

		if (!color) {
			for (x = 0; x < width; x++, p += 4, dst += 3) {
				dst[0] = p[0];
				dst[1] = p[1];
				dst[2] = p[2];
			}
			continue;
		}
		for (x = 0; x < width; x++, p += 4, dst += 3) {
			s32 b = p[0], g = p[1], r = p[2];

			apply_color(color, &r, &g, &b);
			dst[0] = b;
			dst[1] = g;
			dst[2] = r;
		}
	}
}

/*
 * Convert one 16x16 MCU to level-shifted YCbCr samples (JFIF / BT.601 full
 * range, 16.16 fixed point).  Chroma is subsampled by averaging 2x2 pixels;
 * because the conversion is linear, each pixel's weighted RGB is summed and
 * the 2x2 sum is scaled and rounded once at the end.
 *
 * If @color is set, each pixel first goes through colour management.
 *
 * Y goes into @y[0..3] (top-left, top-right, bottom-left, bottom-right) as
 * s16 (it fits in -128..127), Cb and Cr into @cb and @cr; natural order.
 * Kept this small because kernel builds warn above 1024 bytes of stack.
 */
static void load_mcu(const u8 *pixels, unsigned int pitch, unsigned int width,
		     unsigned int height, unsigned int mx, unsigned int my,
		     const struct t6_jpeg_color *color, s16 y[4][64],
		     s32 cb[64], s32 cr[64])
{
	unsigned int x0 = mx * T6_JPEG_MCU, y0 = my * T6_JPEG_MCU;
	unsigned int yy, xx, k;

	for (k = 0; k < 64; k++) {
		cb[k] = 0;
		cr[k] = 0;
	}

	for (yy = 0; yy < T6_JPEG_MCU; yy++) {
		const u8 *line = pixels +
				 (size_t)min(y0 + yy, height - 1) * pitch;
		s16 *yblk = y[(yy >> 3) * 2] + (yy & 7) * 8;
		unsigned int c = (yy >> 1) * 8;

		for (xx = 0; xx < T6_JPEG_MCU; xx++) {
			const u8 *px = line + min(x0 + xx, width - 1) * 4;
			s32 b = px[0], g = px[1], r = px[2];

			if (color)
				apply_color(color, &r, &g, &b);

			yblk[(xx >> 3) * 64 + (xx & 7)] =
				((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128;
			cb[c + (xx >> 1)] += -11059 * r - 21709 * g + 32768 * b;
			cr[c + (xx >> 1)] += 32768 * r - 27439 * g - 5329 * b;
		}
	}

	/* Sums are 4x the average, hence the extra >> 2. */
	for (k = 0; k < 64; k++) {
		cb[k] = (cb[k] + (1 << 17)) >> 18;
		cr[k] = (cr[k] + (1 << 17)) >> 18;
	}
}

/* --- Forward DCT -------------------------------------------------------- */

#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))

#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

/*
 * One 8-point DCT pass over d[0], d[stride], ... d[7*stride].  @pass is 1 for
 * rows (which keep PASS1_BITS of extra precision) and 2 for columns.
 */
static inline void fdct_1d(s32 *d, unsigned int stride, int pass)
{
	s32 tmp0 = d[0 * stride] + d[7 * stride];
	s32 tmp7 = d[0 * stride] - d[7 * stride];
	s32 tmp1 = d[1 * stride] + d[6 * stride];
	s32 tmp6 = d[1 * stride] - d[6 * stride];
	s32 tmp2 = d[2 * stride] + d[5 * stride];
	s32 tmp5 = d[2 * stride] - d[5 * stride];
	s32 tmp3 = d[3 * stride] + d[4 * stride];
	s32 tmp4 = d[3 * stride] - d[4 * stride];
	s32 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
	s32 tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
	s32 z1, z2, z3, z4, z5;
	int shift = pass == 1 ? CONST_BITS - PASS1_BITS
			      : CONST_BITS + PASS1_BITS;

	/* Even part. */
	if (pass == 1) {
		d[0 * stride] = (tmp10 + tmp11) * (1 << PASS1_BITS);
		d[4 * stride] = (tmp10 - tmp11) * (1 << PASS1_BITS);
	} else {
		d[0 * stride] = DESCALE(tmp10 + tmp11, PASS1_BITS);
		d[4 * stride] = DESCALE(tmp10 - tmp11, PASS1_BITS);
	}
	z1 = (tmp12 + tmp13) * FIX_0_541196100;
	d[2 * stride] = DESCALE(z1 + tmp13 * FIX_0_765366865, shift);
	d[6 * stride] = DESCALE(z1 - tmp12 * FIX_1_847759065, shift);

	/* Odd part. */
	z1 = tmp4 + tmp7;
	z2 = tmp5 + tmp6;
	z3 = tmp4 + tmp6;
	z4 = tmp5 + tmp7;
	z5 = (z3 + z4) * FIX_1_175875602;

	tmp4 *= FIX_0_298631336;
	tmp5 *= FIX_2_053119869;
	tmp6 *= FIX_3_072711026;
	tmp7 *= FIX_1_501321110;
	z1 *= -FIX_0_899976223;
	z2 *= -FIX_2_562915447;
	z3 = z3 * -FIX_1_961570560 + z5;
	z4 = z4 * -FIX_0_390180644 + z5;

	d[7 * stride] = DESCALE(tmp4 + z1 + z3, shift);
	d[5 * stride] = DESCALE(tmp5 + z2 + z4, shift);
	d[3 * stride] = DESCALE(tmp6 + z2 + z3, shift);
	d[1 * stride] = DESCALE(tmp7 + z1 + z4, shift);
}

/* In-place 2-D DCT; output is scaled up by 8 relative to the true DCT. */
static void fdct_8x8(s32 *data)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		fdct_1d(data + i * 8, 1, 1);
	for (i = 0; i < 8; i++)
		fdct_1d(data + i, 8, 2);
}

/* DCT + quantise one block into zigzag order. */
static void transform_block(const struct t6_jpeg_enc *enc, unsigned int table,
			    s32 *data, s16 *out)
{
	const u32 *recip = enc->recip[table];
	const u16 *bias = enc->bias[table];
	unsigned int k;

	fdct_8x8(data);
	for (k = 0; k < 64; k++) {
		s32 v = data[zigzag_to_natural[k]];
		u32 a = v < 0 ? -v : v;
		s32 q = ((a + bias[k]) * recip[k]) >> 20;

		q = min(q, k ? AC_MAX : DC_MAX);
		out[k] = v < 0 ? -q : q;
	}
}

static void transform_mcu(const struct t6_jpeg_enc *enc, const u8 *pixels,
			  unsigned int pitch, unsigned int width,
			  unsigned int height, unsigned int mx,
			  unsigned int my,
			  const struct t6_jpeg_color *color,
			  s16 out[T6_JPEG_BLOCKS_PER_MCU][64])
{
	s32 cb[64], cr[64], data[64];
	unsigned int b, k;

	/* Luma is staged in @out[0..3] and transformed through @data. */
	load_mcu(pixels, pitch, width, height, mx, my, color, out, cb, cr);
	for (b = 0; b < 4; b++) {
		for (k = 0; k < 64; k++)
			data[k] = out[b][k];
		transform_block(enc, 0, data, out[b]);
	}
	transform_block(enc, 1, cb, out[4]);
	transform_block(enc, 1, cr, out[5]);
}

/* --- Entropy coding ----------------------------------------------------- */

/* Magnitude category (T.81 F.1.2.1) and its appended bits. */
static inline void put_value(struct bitwriter *w, s32 v, unsigned int cat)
{
	if (v < 0)
		v -= 1;
	bw_put(w, v, cat);
}

static void encode_block(const struct t6_jpeg_enc *enc, struct bitwriter *w,
			 const s16 *zz, s16 *pred, unsigned int table)
{
	const u16 *ac_code = enc->ac_code[table];
	const u8 *ac_size = enc->ac_size[table];
	s32 diff = zz[0] - *pred;
	unsigned int cat, k, run = 0;

	*pred = zz[0];
	cat = fls(diff < 0 ? -diff : diff);
	bw_put(w, enc->dc_code[table][cat], enc->dc_size[table][cat]);
	put_value(w, diff, cat);

	for (k = 1; k < 64; k++) {
		s32 v = zz[k];
		unsigned int sym;

		if (!v) {
			run++;
			continue;
		}
		while (run > 15) {
			bw_put(w, ac_code[AC_ZRL], ac_size[AC_ZRL]);
			run -= 16;
		}
		cat = fls(v < 0 ? -v : v);
		sym = (run << 4) | cat;
		bw_put(w, ac_code[sym], ac_size[sym]);
		put_value(w, v, cat);
		run = 0;
	}
	if (run)
		bw_put(w, ac_code[AC_EOB], ac_size[AC_EOB]);
}

/* Code one MCU, updating the Y/Cb/Cr DC predictors in @pred. */
static void encode_mcu(const struct t6_jpeg_enc *enc, struct bitwriter *w,
		       const s16 mcu[T6_JPEG_BLOCKS_PER_MCU][64], s16 pred[3])
{
	unsigned int b;

	for (b = 0; b < 4; b++)
		encode_block(enc, w, mcu[b], &pred[0], 0);
	encode_block(enc, w, mcu[4], &pred[1], 1);
	encode_block(enc, w, mcu[5], &pred[2], 1);
}

int t6_jpeg_encode_row(const struct t6_jpeg_enc *enc, const u8 *pixels,
		       unsigned int pitch, unsigned int width,
		       unsigned int height, unsigned int mcu_row,
		       const struct t6_jpeg_color *color,
		       struct t6_jpeg_row *row, size_t tail_cap)
{
	unsigned int cols = t6_jpeg_mcu_cols(width), mx;
	s16 mcu[T6_JPEG_BLOCKS_PER_MCU][64];
	struct bitwriter w;
	s16 pred[3];

	transform_mcu(enc, pixels, pitch, width, height, 0, mcu_row, color,
		      row->mcu0);
	pred[0] = row->mcu0[3][0];
	pred[1] = row->mcu0[4][0];
	pred[2] = row->mcu0[5][0];

	bw_init(&w, row->tail, tail_cap, false);
	for (mx = 1; mx < cols; mx++) {
		transform_mcu(enc, pixels, pitch, width, height, mx, mcu_row,
			      color, mcu);
		encode_mcu(enc, &w, mcu, pred);
		if (w.overflow)
			return -ENOSPC;
	}
	/* Leave the partial byte in place; tail_bits records its length. */
	row->tail_bits = (u32)(w.p - row->tail) * 8 + w.nbits;
	if (w.nbits) {
		if (w.p >= w.end)
			return -ENOSPC;
		*w.p = (u8)(w.acc << (8 - w.nbits));
	}
	memcpy(row->last_dc, pred, sizeof(pred));
	return 0;
}

/* --- File structure ----------------------------------------------------- */

static u8 *put_marker(u8 *p, u8 marker, unsigned int payload_len)
{
	*p++ = 0xff;
	*p++ = marker;
	*p++ = (payload_len + 2) >> 8;
	*p++ = (payload_len + 2) & 0xff;
	return p;
}

static u8 *put_dht(u8 *p, u8 class_id, const u8 bits[16], const u8 *vals)
{
	unsigned int i, n = 0;

	for (i = 0; i < 16; i++)
		n += bits[i];
	*p++ = class_id;
	memcpy(p, bits, 16);
	p += 16;
	memcpy(p, vals, n);
	return p + n;
}

size_t t6_jpeg_write_header(const struct t6_jpeg_enc *enc, unsigned int width,
			    unsigned int height, u8 *out)
{
	static const u8 jfif[14] = {
		'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0
	};
	u8 *p = out;
	unsigned int t;

	*p++ = 0xff;
	*p++ = 0xd8;				/* SOI */

	p = put_marker(p, 0xe0, sizeof(jfif));	/* APP0 JFIF 1.01 */
	memcpy(p, jfif, sizeof(jfif));
	p += sizeof(jfif);

	p = put_marker(p, 0xdb, 2 * 65);	/* DQT, 8-bit tables 0 and 1 */
	for (t = 0; t < 2; t++) {
		*p++ = t;
		memcpy(p, enc->dqt[t], 64);
		p += 64;
	}

	p = put_marker(p, 0xc0, 15);		/* SOF0 baseline */
	*p++ = 8;				/* sample precision */
	*p++ = height >> 8;
	*p++ = height & 0xff;
	*p++ = width >> 8;
	*p++ = width & 0xff;
	*p++ = 3;				/* components */
	*p++ = 1; *p++ = 0x22; *p++ = 0;	/* Y:  2x2 sampling, table 0 */
	*p++ = 2; *p++ = 0x11; *p++ = 1;	/* Cb: 1x1, table 1 */
	*p++ = 3; *p++ = 0x11; *p++ = 1;	/* Cr: 1x1, table 1 */

	p = put_marker(p, 0xc4, 4 * 17 + 2 * 12 + 2 * 162); /* DHT */
	p = put_dht(p, 0x00, dc_bits[0], dc_vals);
	p = put_dht(p, 0x10, ac_bits[0], ac_vals[0]);
	p = put_dht(p, 0x01, dc_bits[1], dc_vals);
	p = put_dht(p, 0x11, ac_bits[1], ac_vals[1]);

	p = put_marker(p, 0xda, 10);		/* SOS */
	*p++ = 3;
	*p++ = 1; *p++ = 0x00;			/* Y:  DC0/AC0 */
	*p++ = 2; *p++ = 0x11;			/* Cb: DC1/AC1 */
	*p++ = 3; *p++ = 0x11;			/* Cr: DC1/AC1 */
	*p++ = 0;				/* Ss */
	*p++ = 63;				/* Se */
	*p++ = 0;				/* Ah/Al */

	return p - out;
}

long t6_jpeg_assemble(const struct t6_jpeg_enc *enc, unsigned int width,
		      unsigned int height, const struct t6_jpeg_row *rows,
		      u8 *out, size_t cap)
{
	unsigned int nrows = t6_jpeg_mcu_rows(height), r;
	struct bitwriter w;
	s16 pred[3] = { 0, 0, 0 };
	size_t hdr;

	if (cap < T6_JPEG_HEADER_LEN + 2)
		return -ENOSPC;
	hdr = t6_jpeg_write_header(enc, width, height, out);

	/* Leave room for the EOI marker. */
	bw_init(&w, out + hdr, cap - hdr - 2, true);
	for (r = 0; r < nrows && !w.overflow; r++) {
		encode_mcu(enc, &w, rows[r].mcu0, pred);
		bw_put_stream(&w, rows[r].tail, rows[r].tail_bits);
		memcpy(pred, rows[r].last_dc, sizeof(pred));
	}
	bw_flush(&w);
	if (w.overflow)
		return -ENOSPC;

	*w.p++ = 0xff;
	*w.p++ = 0xd9;				/* EOI */
	return w.p - out;
}
