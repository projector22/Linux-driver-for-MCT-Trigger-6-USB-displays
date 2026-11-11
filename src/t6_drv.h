/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * MCT Trigger 6 USB display driver: shared definitions.
 *
 * Source layout:
 *   t6_drv.c     USB driver glue: probe, disconnect, suspend/resume, params
 *   t6_usb.c     USB transport: vendor control requests, bulk objects,
 *                interrupt endpoint (hot-plug and frame-done events)
 *   t6_kms.c     DRM/KMS objects: planes, CRTCs, encoders, connectors,
 *                mode handling and the vblank timer
 *   t6_cursor.c  hardware cursor plane
 *   t6_stream.c  frame pipeline: damage tracking, JPEG encoding, pacing
 *   t6_proto.c   wire formats (hardware independent, host-tested)
 *   t6_jpeg.c    baseline JPEG encoder (hardware independent, host-tested)
 */
#ifndef T6_DRV_H
#define T6_DRV_H

#include <linux/mutex.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

#include "t6_jpeg.h"
#include "t6_proto.h"

#define DRIVER_NAME		"mct_t6"
#define DRIVER_DESC		"MCT Trigger 6 USB display"

/* Encoder work items per frame; bounded by online CPUs at run time. */
#define T6_MAX_ENCODE_JOBS	8

/* Bulk payloads are split into URBs of this size and queued together. */
#define T6_BULK_CHUNK		(128 * 1024)
#define T6_BULK_URBS		DIV_ROUND_UP(T6_RING_MAX_OBJECT, T6_BULK_CHUNK)

struct t6_device;
struct t6_head;

/* Module parameters (t6_drv.c). */
extern unsigned int t6_param_quality;
extern unsigned int t6_param_encode_jobs;
extern bool t6_param_hw_cursor;
extern int t6_param_trust_damage;
extern int t6_param_lossless;
extern unsigned int t6_param_lossless_delay_ms;
extern bool t6_param_lossless_always;
extern unsigned int t6_param_lossless_min_fps;

struct t6_stats {
	u64 frames;
	u64 raw_frames;		/* of @frames, lossless RGB ones */
	u64 bytes;
	u64 rows_encoded;
	u64 encode_ns;
	u64 send_ns;
	u64 dropped;		/* frames abandoned after a USB error */
	u64 fence_timeouts;
	u32 last_frame_bytes;
};

struct t6_encode_job {
	struct work_struct work;
	struct t6_stream *stream;
	const u16 *rows;	/* MCU row indices to encode */
	unsigned int count;
	int err;
};

/*
 * Per-head frame pipeline.  KMS writes damaged pixels into @shadow and marks
 * MCU rows in @dirty; the worker snapshots dirty rows into @pixels, encodes
 * only those rows, assembles a JPEG from the row cache and sends it.
 */
struct t6_stream {
	struct t6_head *head;
	struct delayed_work work;

	/* Producer side, protected by @lock. */
	struct mutex lock;
	bool active;
	unsigned int width, height;
	u8 *shadow;
	unsigned long *dirty;
	u64 damage_ns;		/* when damage last arrived */
	/* From the CRTC's CTM and GAMMA_LUT; kept across mode sets. */
	struct t6_jpeg_color color;
	struct t6_jpeg_color color_new;	/* scratch for t6_stream_set_color() */

	/* Worker side; only touched by @work (or with it stopped). */
	u8 *pixels;
	struct t6_jpeg_color enc_color;	/* what the rows being encoded use */
	unsigned int nrows;
	unsigned long *todo;
	u16 *todo_list;
	struct t6_jpeg_row *rows;
	size_t *row_cap;
	struct t6_jpeg_enc *enc;
	unsigned int quality;
	unsigned int over_budget, under_budget;
	u8 *frame;		/* video header + JPEG + padding */
	struct t6_vram_layout vram;	/* head's layout, ring shortened if raw */
	struct t6_stream_pos pos;

	/*
	 * Lossless refinement (t6_param_lossless): while the picture changes,
	 * frames are JPEG; once it has been quiet for lossless_delay_ms a raw
	 * RGB frame replaces the last JPEG.  "Quiet" is measured between
	 * damage arrivals, so it doesn't depend on how long frames take to
	 * send.  @jpeg_stale holds rows that went
	 * out raw and so still need re-encoding before the next JPEG.
	 */
	bool raw_ok;		/* raw frames fit in VRAM for this mode */
	unsigned int raw_failures;	/* consecutive */
	bool refine;		/* the frame on screen is a JPEG */
	u64 prev_damage_ns;	/* damage_ns of the last batch sent */
	/*
	 * Motion goes out raw while raw frames, on average @raw_cost_ns
	 * each, still reach lossless_min_fps (t6_raw_motion_next()).
	 */
	u64 raw_cost_ns;	/* running average, 0 = not yet measured */
	bool raw_motion;
	struct t6_raw_layout raw;
	unsigned int raw_index;
	u8 *raw_frame;		/* video header + BGR pixels */
	unsigned long *jpeg_stale;
	struct t6_encode_job jobs[T6_MAX_ENCODE_JOBS];

	struct t6_stats stats;	/* updated by the worker, read by debugfs */
};

struct t6_cursor_state {
	bool uploaded;		/* the chip holds the current shape */
	bool visible;
	u8 slot;		/* shape slot being shown (double-buffered) */
};

struct t6_head {
	struct t6_device *t6;
	unsigned int index;		/* protocol output index, 0 or 1 */
	u8 link;			/* T6_LINK_* */

	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct hrtimer vblank_timer;
	ktime_t vblank_period;
	bool vblank_enabled;
	ktime_t output_off_time;	/* when the output was last switched off */

	/* The chip's own mode table for this head (read once at probe). */
	struct t6_timing *timings;
	unsigned int num_timings;

	struct t6_vram_layout vram;
	struct t6_stream stream;
	struct t6_cursor_state cursor_state;
	bool damage_untrusted;		/* compare frames until the next mode set */
};

struct t6_device {
	struct drm_device drm;
	struct usb_device *udev;
	struct usb_interface *intf;
	struct device *dmadev;
	struct usb_endpoint_descriptor *ep_bulk_out;
	struct usb_endpoint_descriptor *ep_int_in;

	unsigned int vram_mb;
	u32 hw_platform;
	u32 fw_version;
	unsigned int num_heads;
	struct t6_head heads[T6_MAX_HEADS];

	struct workqueue_struct *wq;	/* frame workers (unbound) */
	struct dentry *debugfs;

	/* Control transfers: a DMA-safe bounce buffer and its lock. */
	struct mutex ctrl_lock;
	u8 *ctrl_buf;

	/* Bulk OUT: one object (header + payload) at a time. */
	struct mutex bulk_lock;
	u8 *bulk_hdr;
	u8 *bulk_chunk[T6_BULK_URBS];
	struct urb *bulk_urb[T6_BULK_URBS];
	struct usb_anchor bulk_anchor;

	/* Interrupt IN. */
	struct urb *int_urb;
	u8 *int_buf;
	struct work_struct hotplug_work;

	/*
	 * Frame-done fences: every video object carries a sequence number
	 * which the chip echoes when it has finished with it.  Used to keep
	 * at most a couple of frames in flight.
	 */
	atomic_t fence_sent;
	atomic_t fence_done;
	bool fences_seen;
	wait_queue_head_t fence_wq;
};

static inline struct t6_device *to_t6(struct drm_device *drm)
{
	return container_of(drm, struct t6_device, drm);
}

/* t6_usb.c */
int t6_usb_init(struct t6_device *t6);
void t6_usb_fini(struct t6_device *t6);
int t6_ctrl_out(struct t6_device *t6, u8 request, u16 value, u16 index,
		const void *data, u16 len);
int t6_ctrl_in(struct t6_device *t6, u8 request, u16 value, u16 index,
	       void *data, u16 len);
int t6_ctrl_in_exact(struct t6_device *t6, u8 request, u16 value, u16 index,
		     void *data, u16 len);
int t6_send_object(struct t6_device *t6, u32 addr, const u8 *buf, u32 len);
int t6_int_start(struct t6_device *t6);
void t6_int_stop(struct t6_device *t6);
int t6_wait_fences(struct t6_device *t6, unsigned int max_inflight);

/* t6_kms.c */
int t6_kms_init(struct t6_device *t6);
const struct t6_timing *t6_find_timing(const struct t6_head *head,
				       const struct drm_display_mode *mode);

/* t6_cursor.c */
int t6_cursor_plane_init(struct t6_head *head);

/* t6_stream.c */
void t6_stream_init(struct t6_head *head);
int t6_stream_start(struct t6_head *head, unsigned int width,
		    unsigned int height);
void t6_stream_stop(struct t6_head *head);
void t6_stream_update(struct t6_head *head, const void *src,
		      unsigned int src_pitch, const struct drm_rect *clip);
void t6_stream_update_changed(struct t6_head *head, const void *src,
			      unsigned int src_pitch);
void t6_stream_set_color(struct t6_head *head, const u64 *ctm,
			 const u16 *gamma_lut);
void t6_stream_fini(struct t6_head *head);

#endif /* T6_DRV_H */
