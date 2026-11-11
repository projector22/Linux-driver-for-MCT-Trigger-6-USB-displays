/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Baseline JPEG encoder for the MCT Trigger 6 hardware JPEG decoder.
 *
 * Over a USB 2.0 link the Trigger 6 cannot receive uncompressed frames fast
 * enough (1080p60 XRGB is ~370 MB/s, the link manages ~35 MB/s), so frames
 * are sent as baseline JPEG and decoded on the chip.  That is what MCT's own
 * drivers do on non-SuperSpeed links.  The kernel has no software JPEG
 * encoder, so this file provides a small one:
 *
 *   - baseline sequential DCT, 8-bit, Huffman-coded with the ITU-T T.81
 *     Annex K example tables (no per-frame table optimisation pass);
 *   - YCbCr 4:2:0 (one 16x16 MCU = four Y blocks + one Cb + one Cr), which is
 *     what MCT's reference driver produces;
 *   - integer-only arithmetic, so no FPU/SIMD context is needed in-kernel;
 *   - no restart markers, so any baseline decoder can consume the output.
 *
 * Incremental encoding
 * --------------------
 * A desktop usually changes only a small part of the screen between frames.
 * To avoid re-encoding the whole frame, the image is encoded one MCU row
 * (16 pixel lines) at a time into a struct t6_jpeg_row, and a frame is then
 * *assembled* from the cached rows.  Rows are independent except for the DC
 * predictor, which in a scan without restart markers runs across row
 * boundaries.  Each cached row therefore stores:
 *
 *   - the quantised coefficients of its first MCU (whose DC differences
 *     depend on the previous row), and
 *   - the entropy-coded bits of MCUs 1..n-1 ("tail"), whose DC differences
 *     depend only on MCUs within the same row, and
 *   - the DC values its last MCU leaves in the predictors.
 *
 * Assembly re-codes each row's first MCU against the real predictor and then
 * splices the row's tail in at bit granularity.  The result is bit-identical
 * to encoding the whole frame in one pass (the host tests check this).
 *
 * Rows can be encoded concurrently: the encoder state is read-only after
 * t6_jpeg_init(), and each row only touches its own struct t6_jpeg_row.
 */
#ifndef T6_JPEG_H
#define T6_JPEG_H

#include "t6_types.h"

/* Pixels per MCU edge for 4:2:0 subsampling. */
#define T6_JPEG_MCU 16

/* Blocks per MCU: Y0 Y1 Y2 Y3 Cb Cr. */
#define T6_JPEG_BLOCKS_PER_MCU 6

/*
 * Exact size of t6_jpeg_write_header() output: SOI (2) + APP0 (18) +
 * DQT (134) + SOF0 (19) + DHT (420) + SOS (14).
 */
#define T6_JPEG_HEADER_LEN 607

/*
 * Worst-case entropy-coded size of one MCU, in bytes, before 0xFF byte
 * stuffing: per block a DC code (<= 20 bits) plus 63 AC codes (<= 26 bits
 * each) plus EOB, rounded up generously.
 */
#define T6_JPEG_MCU_MAX_BYTES 1280

/* Largest image dimension representable in a SOF0 header. */
#define T6_JPEG_MAX_DIM 65535

/**
 * struct t6_jpeg_enc - immutable encoder configuration
 *
 * Built by t6_jpeg_init() for one quality setting; read-only afterwards so it
 * can be shared by concurrent row encoders.  Index 0 is luma, 1 is chroma.
 */
struct t6_jpeg_enc {
	unsigned int quality;
	u8 dqt[2][64];		/* quantiser, zigzag order, as in the DQT segment */
	u32 recip[2][64];	/* ceil(2^20 / divisor), zigzag order */
	u16 bias[2][64];	/* divisor / 2 for round-to-nearest, zigzag order */
	u16 dc_code[2][12];
	u8 dc_size[2][12];
	u16 ac_code[2][256];
	u8 ac_size[2][256];
};

/**
 * struct t6_jpeg_row - one encoded MCU row, reusable until its pixels change
 * @mcu0:      quantised coefficients of the row's first MCU, zigzag order
 * @last_dc:   DC predictor values (Y, Cb, Cr) after the row's last MCU
 * @tail_bits: number of valid bits in @tail
 * @tail:      Huffman-coded MCUs 1..n-1, MSB first, *without* 0xFF stuffing;
 *             storage is owned by the caller
 */
struct t6_jpeg_row {
	s16 mcu0[T6_JPEG_BLOCKS_PER_MCU][64];
	s16 last_dc[3];
	u32 tail_bits;
	u8 *tail;
};

void t6_jpeg_init(struct t6_jpeg_enc *enc, unsigned int quality);

/*
 * Colour management, applied to each pixel before conversion to YCbCr, in
 * the order display hardware uses: the colour transformation matrix (DRM
 * "CTM"), then the per-channel gamma LUT (DRM "GAMMA_LUT").  This is how the
 * driver implements those CRTC properties.
 *
 * The CTM is row-major 3x3 in 16.16 fixed point, out = M * (R, G, B), each
 * output channel clamped to 0..255.
 */
#define T6_JPEG_CTM_ONE		(1 << 16)
#define T6_JPEG_CTM_MAX		(8 * T6_JPEG_CTM_ONE)	/* |coefficient| cap */
#define T6_JPEG_LUT_SIZE	256

struct t6_jpeg_color {
	bool use_ctm;
	bool use_lut;
	s32 ctm[9];
	u8 lut[3][T6_JPEG_LUT_SIZE];	/* R, G, B */
};

/**
 * t6_jpeg_ctm_from_drm() - convert a DRM CTM to the encoder's format
 * @drm: struct drm_color_ctm matrix: S31.32 sign-magnitude, row-major
 * @out: 16.16 coefficients, each clamped to +/- T6_JPEG_CTM_MAX
 *
 * Return: false if the result is the identity, so no matrix need be applied.
 */
bool t6_jpeg_ctm_from_drm(const u64 drm[9], s32 out[9]);

/**
 * t6_jpeg_lut_from_drm() - convert a DRM gamma LUT to the encoder's format
 * @drm: T6_JPEG_LUT_SIZE struct drm_color_lut entries, i.e. groups of four
 *       u16 (red, green, blue, reserved), 0..0xffff
 * @out: 8-bit tables, R, G, B
 *
 * Return: false if the result is the identity, so no LUT need be applied.
 */
bool t6_jpeg_lut_from_drm(const u16 *drm, u8 out[3][T6_JPEG_LUT_SIZE]);

/* Number of MCU rows/columns covering an image. */
static inline unsigned int t6_jpeg_mcu_rows(unsigned int height)
{
	return DIV_ROUND_UP(height, T6_JPEG_MCU);
}

static inline unsigned int t6_jpeg_mcu_cols(unsigned int width)
{
	return DIV_ROUND_UP(width, T6_JPEG_MCU);
}

/* Size of the scratch buffer t6_jpeg_encode_row() needs for a row's tail. */
static inline size_t t6_jpeg_row_tail_max(unsigned int width)
{
	return (size_t)t6_jpeg_mcu_cols(width) * T6_JPEG_MCU_MAX_BYTES;
}

/*
 * Upper bound of a complete JPEG file for an image, worst case including
 * byte stuffing (which can at most double the entropy-coded size).
 */
static inline size_t t6_jpeg_frame_max(unsigned int width, unsigned int height)
{
	return T6_JPEG_HEADER_LEN + 2 +
	       2 * (size_t)t6_jpeg_mcu_rows(height) * t6_jpeg_row_tail_max(width);
}

/**
 * t6_jpeg_encode_row() - encode one MCU row of an XRGB8888 image
 * @enc:    encoder configuration
 * @pixels: top-left pixel of the image, XRGB8888 little-endian (B,G,R,X)
 * @pitch:  bytes between image lines
 * @width:  image width in pixels (1..65535)
 * @height: image height in pixels (1..65535)
 * @mcu_row: which row of MCUs to encode
 * @color:  colour management to apply first, or NULL for none
 * @row:    output; @row->tail must point to @tail_cap bytes of storage
 * @tail_cap: capacity of @row->tail; t6_jpeg_row_tail_max() always suffices
 *
 * Pixels outside the image (when the size is not a multiple of 16) are
 * filled by replicating the last column/line, which is the conventional way
 * to minimise edge artefacts.
 *
 * Return: 0, or -ENOSPC if @tail_cap is too small.
 */
int t6_jpeg_encode_row(const struct t6_jpeg_enc *enc, const u8 *pixels,
		       unsigned int pitch, unsigned int width,
		       unsigned int height, unsigned int mcu_row,
		       const struct t6_jpeg_color *color,
		       struct t6_jpeg_row *row, size_t tail_cap);

/**
 * t6_jpeg_write_header() - write SOI..SOS for a 4:2:0 baseline image
 * @out: at least T6_JPEG_HEADER_LEN bytes
 *
 * Return: T6_JPEG_HEADER_LEN.
 */
size_t t6_jpeg_write_header(const struct t6_jpeg_enc *enc, unsigned int width,
			    unsigned int height, u8 *out);

/**
 * t6_jpeg_assemble() - build a complete JPEG file from encoded rows
 * @rows: t6_jpeg_mcu_rows(@height) rows, all encoded with @enc
 * @out:  destination
 * @cap:  capacity of @out; t6_jpeg_frame_max() always suffices
 *
 * Return: total file size in bytes, or -ENOSPC if it does not fit in @cap.
 */
long t6_jpeg_assemble(const struct t6_jpeg_enc *enc, unsigned int width,
		      unsigned int height, const struct t6_jpeg_row *rows,
		      u8 *out, size_t cap);

/**
 * t6_color_bgr24() - convert XRGB8888 lines to packed B,G,R for raw frames
 * @src:   first pixel, XRGB8888 little-endian (B,G,R,X)
 * @pitch: bytes between source lines
 * @dst:   output, @width * 3 bytes per line, lines packed
 * @color: colour management to apply, exactly as the encoder does, or NULL
 */
void t6_color_bgr24(const u8 *src, unsigned int pitch, unsigned int width,
		    unsigned int lines, const struct t6_jpeg_color *color,
		    u8 *dst);

#endif /* T6_JPEG_H */
