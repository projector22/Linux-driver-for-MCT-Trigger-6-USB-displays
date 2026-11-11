/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Minimal portability layer for the "pure" parts of the driver.
 *
 * t6_proto.c and t6_jpeg.c contain no kernel-specific logic: they build and
 * parse wire formats and compress pixels.  Keeping them free of kernel APIs
 * lets the exact same source be compiled into the module *and* into the host
 * unit-test binaries under tests/host, where they run under ASan/UBSan and
 * are checked against an independent JPEG decoder.
 *
 * In the kernel this header just pulls in the usual headers.  On the host it
 * provides the handful of kernel types and helpers those files use.
 */
#ifndef T6_TYPES_H
#define T6_TYPES_H

#ifdef __KERNEL__

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#else /* host build */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define min_t(t, a, b) min((t)(a), (t)(b))
#define clamp_t(t, v, lo, hi) clamp((t)(v), (t)(lo), (t)(hi))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define SZ_1M 0x00100000u

/* fls(): 1-based index of the most significant set bit, 0 for 0. */
static inline int fls(unsigned int x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}

/* The wire format is little-endian; the test hosts are too, but be exact. */
static inline void put_unaligned_le16(u16 v, void *p)
{
	u8 *b = p;

	b[0] = v;
	b[1] = v >> 8;
}

static inline void put_unaligned_le32(u32 v, void *p)
{
	u8 *b = p;

	b[0] = v;
	b[1] = v >> 8;
	b[2] = v >> 16;
	b[3] = v >> 24;
}

static inline u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;

	return b[0] | (u16)b[1] << 8;
}

static inline u32 get_unaligned_le32(const void *p)
{
	const u8 *b = p;

	return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

#endif /* __KERNEL__ */

#endif /* T6_TYPES_H */
