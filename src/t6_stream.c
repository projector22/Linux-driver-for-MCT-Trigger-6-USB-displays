// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * Frame pipeline: from KMS damage to JPEG objects on the wire.
 *
 * Producer (KMS commit, t6_stream_update()):
 *   copies the damaged rectangle into @shadow and marks the MCU rows (16
 *   pixel lines) it touches in @dirty, then kicks the worker.  This is a
 *   memcpy of the damage only, so commits never wait for encoding or USB.
 *
 * Worker (t6_stream_work(), one per head on an unbound workqueue):
 *   1. under the lock, moves the dirty bitmap to @todo and copies those rows
 *      from @shadow to @pixels (the encoder's private full-frame snapshot);
 *   2. re-encodes just those rows, spread over several CPUs;
 *   3. assembles a complete JPEG from the per-row cache (t6_jpeg.h);
 *   4. places it in the chip's ring and sends it.
 *
 * If frames arrive faster than they can be sent, damage simply accumulates
 * in @dirty and is sent with the next frame: the newest content always wins
 * and nothing queues up.
 *
 * Quality adapts to the link: frames persistently over the per-frame byte
 * budget lower the JPEG quality, persistently small frames raise it back
 * towards the configured maximum.
 *
 * Lossless refinement (by default on SuperSpeed links): JPEG's 4:2:0 colour
 * and quantisation blur fine text, so once the picture has been still for
 * lossless_delay_ms the worker sends the whole frame again as raw RGB24,
 * which the chip scans out untouched.  An update arriving after such a
 * quiet spell goes out raw straight away; rapid updates (scrolling, video)
 * stay JPEG, which is an order of magnitude smaller.
 *
 * Best picture first: rapid updates go out raw as well for as long as raw
 * frames, timed as they are sent, still reach lossless_min_fps.  Only when
 * the link can't manage that (USB 2, two busy heads, a slow host) does
 * motion fall back to JPEG with a raw refinement once things settle, and
 * only when JPEG frames overrun the link does their quality drop.  With
 * lossless_always every frame is raw, however slow.
 */
#include <linux/bitmap.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "t6_drv.h"

/* JPEG bytes that fit in one ring object next to the header and padding. */
#define T6_JPEG_CAP	(T6_RING_MAX_OBJECT - T6_VIDEO_HDR_LEN - T6_JPEG_PAD)

/* Frames the chip may still be decoding before we wait, per active head. */
#define T6_MAX_INFLIGHT	2

/* Back-off before re-sending after a failed frame. */
#define T6_RETRY_DELAY	(HZ / 10)

#define T6_QUALITY_MIN	30
#define T6_QUALITY_STEP	5

/* Raw frames that fail in a row before lossless mode is given up. */
#define T6_RAW_MAX_FAILURES	3

/* Consecutive frames over/under budget before quality changes. */
#define T6_OVER_BUDGET_FRAMES	3
#define T6_UNDER_BUDGET_FRAMES	120

static unsigned int t6_frame_budget(const struct t6_device *t6)
{
	/*
	 * High-speed USB moves ~35 MB/s of bulk data in practice; budget for
	 * 60 frames per second.  SuperSpeed has an order of magnitude more,
	 * so there the cap is effectively only the ring object size.
	 */
	if (t6->udev->speed >= USB_SPEED_SUPER)
		return T6_JPEG_CAP;
	return 35 * 1000 * 1000 / 60;
}

/* --- Row encoding ------------------------------------------------------- */

static int t6_encode_one_row(struct t6_stream *s, unsigned int r)
{
	struct t6_jpeg_row *row = &s->rows[r];
	const struct t6_jpeg_color *color =
		s->enc_color.use_ctm || s->enc_color.use_lut ? &s->enc_color :
							       NULL;
	unsigned int pitch = s->width * 4;
	size_t cap;
	u8 *buf;
	int ret;

	ret = t6_jpeg_encode_row(s->enc, s->pixels, pitch, s->width, s->height,
				 r, color, row, s->row_cap[r]);
	if (ret != -ENOSPC)
		return ret;

	/* Busy content: grow this row's buffer to the worst case. */
	cap = t6_jpeg_row_tail_max(s->width);
	buf = kvmalloc(cap, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	kvfree(row->tail);
	row->tail = buf;
	s->row_cap[r] = cap;

	return t6_jpeg_encode_row(s->enc, s->pixels, pitch, s->width,
				  s->height, r, color, row, cap);
}

static void t6_encode_job_run(struct t6_encode_job *job)
{
	unsigned int i;

	job->err = 0;
	for (i = 0; i < job->count && !job->err; i++)
		job->err = t6_encode_one_row(job->stream, job->rows[i]);
}

static void t6_encode_job_work(struct work_struct *work)
{
	t6_encode_job_run(container_of(work, struct t6_encode_job, work));
}

/* Encode every row in @todo, in parallel where that pays off. */
static int t6_encode_rows(struct t6_stream *s)
{
	unsigned int n = 0, njobs, per, j, r;
	int ret = 0;

	for_each_set_bit(r, s->todo, s->nrows)
		s->todo_list[n++] = r;
	if (!n)
		return 0;

	njobs = t6_param_encode_jobs ? t6_param_encode_jobs : num_online_cpus();
	njobs = clamp_t(unsigned int, njobs, 1, T6_MAX_ENCODE_JOBS);
	/* Splitting a couple of rows across CPUs costs more than it saves. */
	njobs = min(njobs, DIV_ROUND_UP(n, 4));
	per = DIV_ROUND_UP(n, njobs);

	for (j = 0; j < njobs; j++) {
		struct t6_encode_job *job = &s->jobs[j];

		job->rows = s->todo_list + j * per;
		job->count = min(per, n - min(n, j * per));
	}

	/*
	 * Queue all but the first job on the driver's unbound workqueue (the
	 * system unbound queue is deprecated from 6.17); run the first here.
	 */
	for (j = 1; j < njobs; j++)
		if (s->jobs[j].count)
			queue_work(s->head->t6->wq, &s->jobs[j].work);
	t6_encode_job_run(&s->jobs[0]);
	ret = s->jobs[0].err;

	for (j = 1; j < njobs; j++) {
		if (!s->jobs[j].count)
			continue;
		flush_work(&s->jobs[j].work);
		if (!ret)
			ret = s->jobs[j].err;
	}

	s->stats.rows_encoded += n;
	return ret;
}

/* --- Quality control ---------------------------------------------------- */

static void t6_quality_feedback(struct t6_stream *s, unsigned int len)
{
	unsigned int budget = t6_frame_budget(s->head->t6);

	if (len > budget) {
		s->under_budget = 0;
		if (++s->over_budget >= T6_OVER_BUDGET_FRAMES &&
		    s->quality > T6_QUALITY_MIN) {
			s->quality = max(s->quality - T6_QUALITY_STEP,
					 (unsigned int)T6_QUALITY_MIN);
			s->over_budget = 0;
		}
	} else if (len < budget / 2 && s->quality < t6_param_quality) {
		s->over_budget = 0;
		if (++s->under_budget >= T6_UNDER_BUDGET_FRAMES) {
			s->quality = min(s->quality + T6_QUALITY_STEP,
					 t6_param_quality);
			s->under_budget = 0;
		}
	} else {
		s->over_budget = 0;
		s->under_budget = 0;
	}
}

/* Rows encoded at another quality are stale: re-encode everything. */
static void t6_apply_quality(struct t6_stream *s)
{
	if (s->enc->quality == s->quality)
		return;
	t6_jpeg_init(s->enc, s->quality);
	bitmap_fill(s->todo, s->nrows);
}

/* --- Sending ------------------------------------------------------------ */

static int t6_send_frame(struct t6_stream *s, unsigned int jpeg_len)
{
	struct t6_head *head = s->head;
	struct t6_device *t6 = head->t6;
	u32 total = T6_VIDEO_HDR_LEN + jpeg_len + T6_JPEG_PAD;
	struct t6_flip flip = {};
	u32 ring_addr, fb_addr;
	bool reset;
	u64 t0;
	int ret;

	memset(s->frame + T6_VIDEO_HDR_LEN + jpeg_len, 0, T6_JPEG_PAD);

	ret = t6_stream_place(&s->vram, &s->pos, total, &ring_addr,
			      &fb_addr, &reset);
	if (ret)
		return ret;

	if (t6_wait_fences(t6, T6_MAX_INFLIGHT * t6->num_heads))
		s->stats.fence_timeouts++;

	flip.command = head->index ? T6_VIDEO_CMD_FLIP_SECONDARY :
				     T6_VIDEO_CMD_FLIP_PRIMARY;
	flip.payload_size = jpeg_len + T6_JPEG_PAD;
	flip.fence = atomic_inc_return(&t6->fence_sent);
	flip.target_format = T6_FORMAT_NV12;
	flip.y_pitch = t6_nv12_pitch(s->width);
	flip.uv_pitch = flip.y_pitch;
	flip.y_addr = fb_addr;
	flip.uv_addr = fb_addr + t6_nv12_uv_offset(s->width, s->height);
	flip.source_format = T6_FORMAT_JPEG;
	flip.flags = reset ? T6_VIDEO_FLAG_RESET_JPEG : 0;
	t6_flip_encode(&flip, s->frame);

	t0 = ktime_get_ns();
	ret = t6_send_object(t6, ring_addr, s->frame, total);
	s->stats.send_ns += ktime_get_ns() - t0;

	if (ret) {
		/* This fence will never complete; don't let it stall others. */
		atomic_set(&t6->fence_done, flip.fence);
		return ret;
	}
	s->stats.frames++;
	s->stats.bytes += total;
	s->stats.last_frame_bytes = total;
	return 0;
}

/* --- Lossless frames ---------------------------------------------------- */

static bool t6_lossless_now(const struct t6_stream *s)
{
	int mode = READ_ONCE(t6_param_lossless);

	if (!s->raw_ok || !mode)
		return false;
	return mode > 0 || s->head->t6->udev->speed >= USB_SPEED_SUPER;
}

static u64 t6_lossless_delay_ns(void)
{
	return (u64)READ_ONCE(t6_param_lossless_delay_ms) * NSEC_PER_MSEC;
}

/* Should this update, arriving during motion, go out raw? */
static bool t6_raw_for_motion(struct t6_stream *s)
{
	bool was = s->raw_motion;

	if (READ_ONCE(t6_param_lossless_always))
		return true;
	s->raw_motion = t6_raw_motion_next(was, s->raw_cost_ns,
					   READ_ONCE(t6_param_lossless_min_fps));
	if (s->raw_motion != was)
		drm_dbg_driver(&s->head->t6->drm,
			       "head %u: motion now %s (raw frame %llu us)\n",
			       s->head->index,
			       s->raw_motion ? "lossless" : "JPEG",
			       div_u64(s->raw_cost_ns, NSEC_PER_USEC));
	return s->raw_motion;
}

static unsigned long t6_lossless_jiffies(u64 ns)
{
	return max(1UL, nsecs_to_jiffies(ns));
}

/* Send the whole of @pixels as one raw RGB24 frame. */
static int t6_send_raw(struct t6_stream *s)
{
	struct t6_head *head = s->head;
	struct t6_device *t6 = head->t6;
	u32 bytes = t6_raw_pitch(s->width) * s->height;
	u32 total = T6_VIDEO_HDR_LEN + bytes;
	struct t6_flip flip = {};
	u32 fb;
	u64 t0, start;
	int ret;

	start = t0 = ktime_get_ns();
	t6_color_bgr24(s->pixels, s->width * 4, s->width, s->height,
		       &s->enc_color, s->raw_frame + T6_VIDEO_HDR_LEN);
	s->stats.encode_ns += ktime_get_ns() - t0;

	/* Three buffers, like the JPEG targets: never the one on screen. */
	s->raw_index = (s->raw_index + 1) % T6_FB_SLOTS;
	fb = s->raw.fb[s->raw_index];

	if (t6_wait_fences(t6, T6_MAX_INFLIGHT * t6->num_heads))
		s->stats.fence_timeouts++;

	flip.command = head->index ? T6_VIDEO_CMD_FLIP_SECONDARY :
				     T6_VIDEO_CMD_FLIP_PRIMARY;
	flip.payload_size = bytes;
	flip.fence = atomic_inc_return(&t6->fence_sent);
	flip.target_format = T6_FORMAT_RGB24;
	flip.y_pitch = t6_raw_pitch(s->width);
	flip.uv_pitch = 0;
	flip.y_addr = fb;
	flip.uv_addr = fb;
	flip.source_format = T6_FORMAT_RGB24;
	t6_flip_encode(&flip, s->raw_frame);

	t0 = ktime_get_ns();
	ret = t6_send_object(t6, fb - T6_VIDEO_HDR_LEN, s->raw_frame, total);
	s->stats.send_ns += ktime_get_ns() - t0;

	if (ret) {
		atomic_set(&t6->fence_done, flip.fence);
		return ret;
	}
	/* What a raw frame costs end to end decides raw for motion. */
	s->raw_cost_ns = t6_raw_cost_update(s->raw_cost_ns,
					    ktime_get_ns() - start);
	s->stats.frames++;
	s->stats.raw_frames++;
	s->stats.bytes += total;
	s->stats.last_frame_bytes = total;
	return 0;
}

static void t6_stream_work(struct work_struct *work)
{
	struct t6_stream *s = container_of(to_delayed_work(work),
					   struct t6_stream, work);
	struct t6_device *t6 = s->head->t6;
	unsigned int pitch, r, attempt;
	bool damage, lossless;
	long len = -ENOSPC;
	int idx, ret;
	u64 t0, quiet, damage_ns;

	if (!drm_dev_enter(&t6->drm, &idx))
		return;

	mutex_lock(&s->lock);
	damage = s->active && !bitmap_empty(s->dirty, s->nrows);
	if (!damage && !(s->active && s->refine)) {
		mutex_unlock(&s->lock);
		goto out;
	}
	damage_ns = s->damage_ns;
	bitmap_copy(s->todo, s->dirty, s->nrows);
	bitmap_zero(s->dirty, s->nrows);
	/* t6_stream_set_color() marked every row dirty when this changed. */
	s->enc_color = s->color;
	pitch = s->width * 4;
	for_each_set_bit(r, s->todo, s->nrows) {
		unsigned int y = r * T6_JPEG_MCU;
		unsigned int lines = min(T6_JPEG_MCU, s->height - y);

		memcpy(s->pixels + (size_t)y * pitch,
		       s->shadow + (size_t)y * pitch, (size_t)lines * pitch);
	}
	mutex_unlock(&s->lock);

	/*
	 * Quiet time: for new damage, how long the picture had been still
	 * before it arrived; for a pending refinement, how long it has been
	 * still now.  Send times don't count, so a slow link doesn't make
	 * continuous motion look quiet.
	 */
	lossless = t6_lossless_now(s);
	quiet = (damage ? damage_ns : ktime_get_ns()) - s->prev_damage_ns;
	if (damage)
		s->prev_damage_ns = damage_ns;
	if (lossless && (quiet >= t6_lossless_delay_ns() ||
			 (damage && t6_raw_for_motion(s)))) {
		ret = t6_send_raw(s);
		if (!ret) {
			s->raw_failures = 0;
			s->refine = false;
			/* The JPEG row cache doesn't have these rows yet. */
			bitmap_or(s->jpeg_stale, s->jpeg_stale, s->todo,
				  s->nrows);
			goto out;
		}
		drm_err_ratelimited(&t6->drm, "head %u: raw frame failed: %d\n",
				    s->head->index, ret);
		if (++s->raw_failures >= T6_RAW_MAX_FAILURES) {
			drm_warn(&t6->drm,
				 "head %u: lossless frames keep failing; using JPEG only\n",
				 s->head->index);
			s->raw_ok = false;
			s->refine = false;
			lossless = false;
			/* Fall through: send this update as JPEG. */
		} else {
			goto drop;
		}
	}

	if (!damage) {
		/* A refinement came due too early: try again when quiet. */
		if (lossless && s->refine)
			queue_delayed_work(t6->wq, &s->work,
					   t6_lossless_jiffies(t6_lossless_delay_ns() -
							       quiet));
		else
			s->refine = false;
		goto out;
	}

	bitmap_or(s->todo, s->todo, s->jpeg_stale, s->nrows);
	bitmap_zero(s->jpeg_stale, s->nrows);

	t0 = ktime_get_ns();
	for (attempt = 0; attempt < 4; attempt++) {
		t6_apply_quality(s);
		ret = t6_encode_rows(s);
		if (ret) {
			drm_err_ratelimited(&t6->drm, "encode failed: %d\n", ret);
			goto drop;
		}
		len = t6_jpeg_assemble(s->enc, s->width, s->height, s->rows,
				       s->frame + T6_VIDEO_HDR_LEN, T6_JPEG_CAP);
		if (len != -ENOSPC)
			break;
		/* Too big for one ring object: drop quality and retry. */
		s->quality = max(s->quality / 2, (unsigned int)T6_QUALITY_MIN);
	}
	s->stats.encode_ns += ktime_get_ns() - t0;
	if (len < 0)
		goto drop;

	t6_quality_feedback(s, len);

	ret = t6_send_frame(s, len);
	if (ret) {
		drm_err_ratelimited(&t6->drm, "head %u: send failed: %d\n",
				    s->head->index, ret);
		goto drop;
	}
	/* Replace this JPEG with a lossless frame once things go quiet. */
	s->refine = lossless;
	if (lossless)
		queue_delayed_work(t6->wq, &s->work,
				   t6_lossless_jiffies(t6_lossless_delay_ns()));
	goto out;

drop:
	/*
	 * The rows in @todo were not delivered.  Mark them dirty again so the
	 * next frame carries them, rather than leaving stale content on screen.
	 */
	s->stats.dropped++;
	mutex_lock(&s->lock);
	bitmap_or(s->dirty, s->dirty, s->todo, s->nrows);
	if (s->active)
		queue_delayed_work(t6->wq, &s->work, T6_RETRY_DELAY);
	mutex_unlock(&s->lock);
out:
	drm_dev_exit(idx);
}

/* --- Public interface --------------------------------------------------- */

static void t6_stream_free(struct t6_stream *s)
{
	unsigned int r;

	if (s->rows)
		for (r = 0; r < s->nrows; r++)
			kvfree(s->rows[r].tail);
	kvfree(s->rows);
	vfree(s->raw_frame);
	bitmap_free(s->jpeg_stale);
	kfree(s->row_cap);
	kfree(s->todo_list);
	bitmap_free(s->todo);
	bitmap_free(s->dirty);
	kfree(s->enc);
	vfree(s->frame);
	vfree(s->pixels);
	vfree(s->shadow);

	s->rows = NULL;
	s->raw_frame = NULL;
	s->jpeg_stale = NULL;
	s->raw_ok = false;
	s->refine = false;
	s->row_cap = NULL;
	s->todo_list = NULL;
	s->todo = NULL;
	s->dirty = NULL;
	s->enc = NULL;
	s->frame = NULL;
	s->pixels = NULL;
	s->shadow = NULL;
	s->nrows = 0;
}

void t6_stream_init(struct t6_head *head)
{
	struct t6_stream *s = &head->stream;
	unsigned int j;

	s->head = head;
	mutex_init(&s->lock);
	INIT_DELAYED_WORK(&s->work, t6_stream_work);
	for (j = 0; j < T6_MAX_ENCODE_JOBS; j++) {
		s->jobs[j].stream = s;
		INIT_WORK(&s->jobs[j].work, t6_encode_job_work);
	}
}

int t6_stream_start(struct t6_head *head, unsigned int width,
		    unsigned int height)
{
	struct t6_stream *s = &head->stream;
	size_t fb_size = (size_t)width * height * 4;
	size_t tail_init;
	unsigned int r;

	t6_stream_stop(head);

	s->nrows = t6_jpeg_mcu_rows(height);
	s->shadow = vzalloc(fb_size);
	s->pixels = vzalloc(fb_size);
	s->frame = vmalloc(T6_RING_MAX_OBJECT);
	s->enc = kmalloc(sizeof(*s->enc), GFP_KERNEL);
	s->dirty = bitmap_zalloc(s->nrows, GFP_KERNEL);
	s->todo = bitmap_zalloc(s->nrows, GFP_KERNEL);
	s->jpeg_stale = bitmap_zalloc(s->nrows, GFP_KERNEL);
	s->todo_list = kcalloc(s->nrows, sizeof(*s->todo_list), GFP_KERNEL);
	s->row_cap = kcalloc(s->nrows, sizeof(*s->row_cap), GFP_KERNEL);
	s->rows = kvcalloc(s->nrows, sizeof(*s->rows), GFP_KERNEL);
	if (!s->shadow || !s->pixels || !s->frame || !s->enc || !s->dirty ||
	    !s->todo || !s->jpeg_stale || !s->todo_list || !s->row_cap ||
	    !s->rows)
		goto nomem;

	/* Typical desktop rows need a fraction of the worst case. */
	tail_init = max_t(size_t, t6_jpeg_row_tail_max(width) / 8, SZ_4K);
	for (r = 0; r < s->nrows; r++) {
		s->rows[r].tail = kvmalloc(tail_init, GFP_KERNEL);
		if (!s->rows[r].tail)
			goto nomem;
		s->row_cap[r] = tail_init;
	}

	s->quality = t6_param_quality;
	t6_jpeg_init(s->enc, s->quality);
	s->over_budget = 0;
	s->under_budget = 0;

	/*
	 * Reserve raw framebuffers whenever they fit, so the lossless
	 * parameter can be switched at run time.  The ring shrinks, but it
	 * still holds several maximum-size JPEGs.
	 */
	s->vram = head->vram;
	s->raw_index = 0;
	s->raw_failures = 0;
	s->prev_damage_ns = 0;
	s->raw_cost_ns = 0;
	s->raw_motion = true;
	if (!t6_raw_layout(&s->vram, width, height, &s->raw)) {
		s->raw_frame = vmalloc(T6_VIDEO_HDR_LEN +
				       t6_raw_pitch(width) * height);
		s->raw_ok = !!s->raw_frame;
	}
	if (!s->raw_ok)
		s->vram = head->vram;
	t6_stream_reset(&s->vram, &s->pos);

	mutex_lock(&s->lock);
	s->width = width;
	s->height = height;
	bitmap_fill(s->dirty, s->nrows);	/* first frame is complete */
	s->damage_ns = ktime_get_ns();
	s->active = true;
	mutex_unlock(&s->lock);
	return 0;

nomem:
	t6_stream_free(s);
	return -ENOMEM;
}

void t6_stream_stop(struct t6_head *head)
{
	struct t6_stream *s = &head->stream;

	mutex_lock(&s->lock);
	s->active = false;
	mutex_unlock(&s->lock);

	/* Waits for an in-flight frame, including its USB transfer. */
	cancel_delayed_work_sync(&s->work);
	t6_stream_free(s);
}

/*
 * Copy a damaged rectangle into the shadow frame.  @src points at pixel
 * (0, 0) of the CRTC's visible area; @clip is in the same coordinates.
 */
void t6_stream_update(struct t6_head *head, const void *src,
		      unsigned int src_pitch, const struct drm_rect *clip)
{
	struct t6_stream *s = &head->stream;
	struct drm_rect r = *clip;
	unsigned int y, pitch, bytes;

	mutex_lock(&s->lock);
	if (!s->active)
		goto unlock;

	/* Never trust the clip to be inside the mode. */
	r.x1 = max(r.x1, 0);
	r.y1 = max(r.y1, 0);
	r.x2 = min_t(int, r.x2, s->width);
	r.y2 = min_t(int, r.y2, s->height);
	if (r.x1 >= r.x2 || r.y1 >= r.y2)
		goto unlock;

	pitch = s->width * 4;
	bytes = (r.x2 - r.x1) * 4;
	for (y = r.y1; y < r.y2; y++)
		memcpy(s->shadow + (size_t)y * pitch + r.x1 * 4,
		       src + (size_t)y * src_pitch + r.x1 * 4, bytes);

	bitmap_set(s->dirty, r.y1 / T6_JPEG_MCU,
		   (r.y2 - 1) / T6_JPEG_MCU - r.y1 / T6_JPEG_MCU + 1);
	s->damage_ns = ktime_get_ns();
	/* Run now, overriding a pending retry back-off. */
	mod_delayed_work(head->t6->wq, &s->work, 0);
unlock:
	mutex_unlock(&s->lock);
}

/*
 * Like t6_stream_update() for the whole visible area, but copies and marks
 * dirty only the lines whose pixels changed.  For when the damage clips can't
 * be trusted (see t6_damage_trusted()).
 */
void t6_stream_update_changed(struct t6_head *head, const void *src,
			      unsigned int src_pitch)
{
	struct t6_stream *s = &head->stream;
	unsigned int y, pitch;
	bool changed = false;

	mutex_lock(&s->lock);
	if (!s->active)
		goto unlock;

	pitch = s->width * 4;
	for (y = 0; y < s->height; y++) {
		u8 *dst = s->shadow + (size_t)y * pitch;
		const void *line = src + (size_t)y * src_pitch;

		if (!memcmp(dst, line, pitch))
			continue;
		memcpy(dst, line, pitch);
		__set_bit(y / T6_JPEG_MCU, s->dirty);
		changed = true;
	}
	if (changed) {
		s->damage_ns = ktime_get_ns();
		mod_delayed_work(head->t6->wq, &s->work, 0);
	}
unlock:
	mutex_unlock(&s->lock);
}

/*
 * Set colour management from the CRTC's CTM matrix (struct drm_color_ctm)
 * and gamma LUT (T6_JPEG_LUT_SIZE struct drm_color_lut entries); NULL for
 * none.  Every cached row was encoded with the old settings, so a change
 * re-sends the whole frame.
 */
void t6_stream_set_color(struct t6_head *head, const u64 *ctm,
			 const u16 *gamma_lut)
{
	struct t6_stream *s = &head->stream;
	struct t6_jpeg_color *c = &s->color_new;

	mutex_lock(&s->lock);
	memset(c, 0, sizeof(*c));
	if (ctm)
		c->use_ctm = t6_jpeg_ctm_from_drm(ctm, c->ctm);
	if (!c->use_ctm)
		memset(c->ctm, 0, sizeof(c->ctm));
	if (gamma_lut)
		c->use_lut = t6_jpeg_lut_from_drm(gamma_lut, c->lut);
	if (!c->use_lut)
		memset(c->lut, 0, sizeof(c->lut));

	if (!memcmp(c, &s->color, sizeof(*c)))
		goto unlock;
	s->color = *c;
	if (s->active) {
		bitmap_fill(s->dirty, s->nrows);
		s->damage_ns = ktime_get_ns();
		mod_delayed_work(head->t6->wq, &s->work, 0);
	}
unlock:
	mutex_unlock(&s->lock);
}

void t6_stream_fini(struct t6_head *head)
{
	t6_stream_stop(head);
	mutex_destroy(&head->stream.lock);
}
