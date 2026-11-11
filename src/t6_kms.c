// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * KMS for the MCT Trigger 6: one primary plane, cursor plane, CRTC, encoder
 * and connector per video output ("head").
 *
 * Modes: the chip can only drive timings from its own firmware table (the
 * PLL settings are tied to them), so a mode is valid only if the table has
 * an entry with the same resolution and refresh rate.  EDID modes are
 * filtered against that, which is also what MCT's drivers do.
 *
 * Vblank: the chip gives no scanout timing, so vblank events come from an
 * hrtimer running at the mode's refresh rate.  That paces compositors to
 * the display rate instead of letting them render as fast as commits are
 * accepted.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "t6_compat.h"
#include "t6_drv.h"

static const u32 t6_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,	/* alpha ignored; some clients only offer it */
};

static const u64 t6_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/* --- Mode table --------------------------------------------------------- */

/*
 * Pick the firmware timing for a mode: same active size and refresh, and if
 * the table offers several (e.g. normal and reduced blanking), the one whose
 * totals match the requested mode.
 */
const struct t6_timing *t6_find_timing(const struct t6_head *head,
				       const struct drm_display_mode *mode)
{
	const struct t6_timing *best = NULL;
	int refresh = drm_mode_vrefresh(mode);
	unsigned int i;

	for (i = 0; i < head->num_timings; i++) {
		const struct t6_timing *t = &head->timings[i];

		if (t->hdisplay != mode->hdisplay ||
		    t->vdisplay != mode->vdisplay ||
		    abs((int)t->refresh - refresh) > 1)
			continue;
		if (t->htotal == mode->htotal && t->vtotal == mode->vtotal)
			return t;
		if (!best)
			best = t;
	}
	return best;
}

static enum drm_mode_status t6_mode_valid(struct t6_head *head,
					  const struct drm_display_mode *mode)
{
	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return MODE_NO_INTERLACE;
	if (!t6_mode_fits_vram(mode->hdisplay, mode->vdisplay))
		return MODE_MEM;
	if (!t6_find_timing(head, mode))
		return MODE_NOMODE;
	return MODE_OK;
}

static void t6_timing_to_mode(const struct t6_timing *t,
			      struct drm_display_mode *mode)
{
	mode->clock = t->clock_khz;
	mode->hdisplay = t->hdisplay;
	mode->hsync_start = t->hsync_start;
	mode->hsync_end = t->hsync_start + t->hsync_width;
	mode->htotal = t->htotal;
	mode->vdisplay = t->vdisplay;
	mode->vsync_start = t->vsync_start;
	mode->vsync_end = t->vsync_start + t->vsync_width;
	mode->vtotal = t->vtotal;
	mode->flags = (t->hsync_positive ? DRM_MODE_FLAG_PHSYNC :
					   DRM_MODE_FLAG_NHSYNC) |
		      (t->vsync_positive ? DRM_MODE_FLAG_PVSYNC :
					   DRM_MODE_FLAG_NVSYNC);
	mode->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);
}

static bool t6_has_usable_mode(struct t6_head *head)
{
	struct drm_display_mode *mode;

	list_for_each_entry(mode, &head->connector.probed_modes, head)
		if (t6_mode_valid(head, mode) == MODE_OK)
			return true;
	return false;
}

/* Offer the chip's own table up to 1080p. */
static int t6_add_table_modes(struct t6_head *head)
{
	struct drm_connector *connector = &head->connector;
	unsigned int i;
	int count = 0;

	for (i = 0; i < head->num_timings; i++) {
		const struct t6_timing *t = &head->timings[i];
		struct drm_display_mode *mode;

		if (t->hdisplay > 1920 || t->vdisplay > 1080)
			continue;
		mode = drm_mode_create(connector->dev);
		if (!mode)
			break;
		t6_timing_to_mode(t, mode);
		if (t->hdisplay == 1024 && t->vdisplay == 768 && t->refresh == 60)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	return count;
}

/* --- Connector ---------------------------------------------------------- */

static int t6_read_edid_block(void *data, u8 *buf, unsigned int block,
			      size_t len)
{
	struct t6_head *head = data;
	int ret;

	if (block >= T6_EDID_MAX_BLOCKS || len != T6_EDID_BLOCK)
		return -EINVAL;
	ret = t6_ctrl_in(head->t6, T6_REQ_GET_EDID, block * T6_EDID_BLOCK,
			 head->index, buf, len);
	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int t6_connector_get_modes(struct drm_connector *connector)
{
	struct t6_head *head = container_of(connector, struct t6_head,
					    connector);
	const struct drm_edid *drm_edid = NULL;
	int idx, count;

	if (drm_dev_enter(connector->dev, &idx)) {
		drm_edid = drm_edid_read_custom(connector, t6_read_edid_block,
						head);
		drm_dev_exit(idx);
	}

	count = t6_edid_connector_update(connector, drm_edid);
	drm_edid_free(drm_edid);

	/*
	 * No EDID, or a monitor (e.g. 4K) none of whose modes this chip can
	 * drive: offer the chip's own modes up to 1080p, as MCT's drivers do.
	 */
	if (!t6_has_usable_mode(head))
		count += t6_add_table_modes(head);
	return count;
}

static enum drm_mode_status
t6_connector_mode_valid(struct drm_connector *connector,
			T6_CONNECTOR_MODE *mode)
{
	return t6_mode_valid(container_of(connector, struct t6_head, connector),
			     mode);
}

static int t6_connector_detect_ctx(struct drm_connector *connector,
				   struct drm_modeset_acquire_ctx *ctx,
				   bool force)
{
	struct t6_head *head = container_of(connector, struct t6_head,
					    connector);
	static const u8 edid_header[8] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
	};
	enum drm_connector_status ret = connector_status_disconnected;
	u8 status = 0, *block;
	int idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;

	if (t6_ctrl_in_exact(head->t6, T6_REQ_GET_CONN_STATUS, head->index, 0,
			     &status, 1))
		ret = connector_status_unknown;
	else if (status == 1)
		ret = connector_status_connected;

	/*
	 * Analogue VGA has no hot-plug detect pin and the status request does
	 * not track it, so look for a monitor answering on DDC instead.  If
	 * none does, report "unknown": MCT's drivers assume VGA is always
	 * present, and video=VGA-1:e can force it on.
	 */
	if (ret != connector_status_connected && head->link == T6_LINK_DAC) {
		ret = connector_status_unknown;
		block = kmalloc(T6_EDID_BLOCK, GFP_KERNEL);
		if (block && !t6_read_edid_block(head, block, 0, T6_EDID_BLOCK) &&
		    !memcmp(block, edid_header, sizeof(edid_header)))
			ret = connector_status_connected;
		kfree(block);
	}

	drm_dev_exit(idx);
	return ret;
}

static const struct drm_connector_helper_funcs t6_connector_helper_funcs = {
	.get_modes = t6_connector_get_modes,
	.mode_valid = t6_connector_mode_valid,
	.detect_ctx = t6_connector_detect_ctx,
};

static const struct drm_connector_funcs t6_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* --- Primary plane ------------------------------------------------------ */

static int t6_primary_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
								      plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (new->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);

	/* No scaling and the plane must cover the whole CRTC. */
	return drm_atomic_helper_check_plane_state(new, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

/*
 * Whether to send only the damage clips, or compare the whole frame instead.
 *
 * Correct userspace keeps clips inside the framebuffer, but the core only
 * rejects clips that are wildly out of range.  X11's modesetting driver
 * reports damage for a rotated PRIME output (e.g. a portrait monitor) in the
 * unrotated layout: 1080x1920 clips on a 1920x1080 framebuffer.  Clipped to
 * the plane, they leave part of the screen that never updates.  A clip
 * outside the framebuffer is the tell: from then until the next mode set,
 * compare frames instead.
 */
static bool t6_damage_trusted(struct t6_head *head,
			      const struct drm_plane_state *state)
{
	const struct drm_mode_rect *clips = drm_plane_get_damage_clips(state);
	unsigned int i, n = drm_plane_get_damage_clips_count(state);
	const struct drm_framebuffer *fb = state->fb;

	if (t6_param_trust_damage >= 0)
		return t6_param_trust_damage;
	if (head->damage_untrusted)
		return false;

	for (i = 0; i < n; i++) {
		if (clips[i].x2 <= (int)fb->width &&
		    clips[i].y2 <= (int)fb->height)
			continue;
		drm_info(head->crtc.dev,
			 "head %u: damage clip %d,%d-%d,%d is outside the %ux%u framebuffer; comparing frames instead\n",
			 head->index, clips[i].x1, clips[i].y1, clips[i].x2,
			 clips[i].y2, fb->width, fb->height);
		head->damage_untrusted = true;
		return false;
	}
	return true;
}

static void t6_primary_atomic_update(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
								      plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state,
								      plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new);
	struct t6_head *head = container_of(plane, struct t6_head, primary);
	struct drm_framebuffer *fb = new->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip, src;
	const u8 *vaddr;
	int idx;

	if (!fb || !new->visible || !new->crtc)
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		return;
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out;

	/* Damage clips are in framebuffer coordinates; ours are CRTC-relative. */
	drm_rect_fp_to_int(&src, &new->src);
	/* data[0] already includes fb->offsets[0]. */
	vaddr = (const u8 *)shadow->data[0].vaddr +
		src.y1 * fb->pitches[0] + src.x1 * fb->format->cpp[0];

	if (!t6_damage_trusted(head, new)) {
		t6_stream_update_changed(head, vaddr, fb->pitches[0]);
		goto end_access;
	}

	drm_atomic_helper_damage_iter_init(&iter, old, new);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		drm_rect_translate(&clip, -src.x1, -src.y1);
		t6_stream_update(head, vaddr, fb->pitches[0], &clip);
	}

end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
out:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs t6_primary_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = t6_primary_atomic_check,
	.atomic_update = t6_primary_atomic_update,
};

static const struct drm_plane_funcs t6_primary_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* --- CRTC --------------------------------------------------------------- */

static enum hrtimer_restart t6_vblank_timer(struct hrtimer *timer)
{
	struct t6_head *head = container_of(timer, struct t6_head,
					    vblank_timer);

	/*
	 * disable_vblank() runs with vblank locks held and so can only *try*
	 * to cancel; stop re-arming here once it has asked us to.
	 */
	if (!READ_ONCE(head->vblank_enabled))
		return HRTIMER_NORESTART;
	drm_crtc_handle_vblank(&head->crtc);
	hrtimer_forward_now(timer, head->vblank_period);
	return HRTIMER_RESTART;
}

static int t6_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct t6_head *head = container_of(crtc, struct t6_head, crtc);

	WRITE_ONCE(head->vblank_enabled, true);
	hrtimer_start(&head->vblank_timer, head->vblank_period,
		      HRTIMER_MODE_REL);
	return 0;
}

static void t6_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct t6_head *head = container_of(crtc, struct t6_head, crtc);

	WRITE_ONCE(head->vblank_enabled, false);
	hrtimer_try_to_cancel(&head->vblank_timer);
}

static enum drm_mode_status t6_crtc_mode_valid(struct drm_crtc *crtc,
					       const struct drm_display_mode *mode)
{
	return t6_mode_valid(container_of(crtc, struct t6_head, crtc), mode);
}

static int t6_crtc_atomic_check(struct drm_crtc *crtc,
				struct drm_atomic_state *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (new->enable && new->mode_changed &&
	    t6_crtc_mode_valid(crtc, &new->mode) != MODE_OK)
		return -EINVAL;
	if (new->gamma_lut &&
	    drm_color_lut_size(new->gamma_lut) != T6_JPEG_LUT_SIZE)
		return -EINVAL;

	/*
	 * Only a modeset needs every plane.  Adding the primary plane to a
	 * cursor-only commit gives it an update without damage clips, which
	 * the damage helpers treat as full damage: every pointer move would
	 * re-encode and re-send the whole frame.
	 */
	if (drm_atomic_crtc_needs_modeset(new))
		return drm_atomic_add_affected_planes(state, crtc);
	return 0;
}

/* MCT's driver lets an output settle this long between off and a new mode. */
#define T6_OUTPUT_OFF_SETTLE_MS	50

/* Program the chip: timing, "software ready", then turn the output on. */
static int t6_head_set_mode(struct t6_head *head,
			    const struct t6_timing *timing)
{
	struct t6_device *t6 = head->t6;
	u8 raw[T6_TIMING_LEN];
	s64 since_off;
	int ret;

	since_off = ktime_ms_delta(ktime_get(), head->output_off_time);
	if (since_off >= 0 && since_off < T6_OUTPUT_OFF_SETTLE_MS)
		msleep(T6_OUTPUT_OFF_SETTLE_MS - since_off);

	t6_timing_encode(timing, raw);
	ret = t6_ctrl_out(t6, T6_REQ_SET_TIMING, head->index, 0, raw,
			  sizeof(raw));
	if (!ret)
		ret = t6_ctrl_out(t6, T6_REQ_SET_SOFTWARE_READY, head->index, 0,
				  NULL, 0);
	if (!ret)
		ret = t6_ctrl_out(t6, T6_REQ_SET_MONITOR_CTRL, head->index, 1,
				  NULL, 0);
	return ret;
}

static void t6_crtc_atomic_enable(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct t6_head *head = container_of(crtc, struct t6_head, crtc);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	const struct t6_timing *timing = t6_find_timing(head, mode);
	int idx, ret;

	head->vblank_period = ns_to_ktime(div_u64(1000000000ULL * mode->htotal *
						  mode->vtotal,
						  mode->clock * 1000ULL));
	drm_crtc_vblank_on(crtc);

	if (!timing || !drm_dev_enter(crtc->dev, &idx))
		return;

	/* A mode set resets the chip's cursor; re-send it on next update. */
	head->cursor_state.uploaded = false;
	head->cursor_state.visible = false;
	/* New mode, maybe new userspace: give its damage clips another chance. */
	head->damage_untrusted = false;

	ret = t6_head_set_mode(head, timing);
	if (ret)
		drm_err(crtc->dev, "head %u: mode set failed: %d\n",
			head->index, ret);
	else if (t6_stream_start(head, mode->hdisplay, mode->vdisplay))
		drm_err(crtc->dev, "head %u: out of memory for %ux%u\n",
			head->index, mode->hdisplay, mode->vdisplay);
	else
		drm_dbg_kms(crtc->dev, "head %u: %ux%u@%u\n", head->index,
			    mode->hdisplay, mode->vdisplay, timing->refresh);
	drm_dev_exit(idx);
}

static void t6_crtc_atomic_disable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct t6_head *head = container_of(crtc, struct t6_head, crtc);
	int idx;

	drm_crtc_vblank_off(crtc);
	t6_stream_stop(head);

	/*
	 * atomic_flush() is not called for a CRTC being switched off, so its
	 * pending event must be completed here or the commit never finishes.
	 */
	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event && !crtc->state->active) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);

	if (!drm_dev_enter(crtc->dev, &idx))
		return;
	t6_ctrl_out(head->t6, T6_REQ_SET_MONITOR_CTRL, head->index, 0, NULL, 0);
	head->output_off_time = ktime_get();
	drm_dev_exit(idx);
}

/*
 * The CTM and GAMMA_LUT properties: applied in software by the JPEG encoder,
 * before the RGB to YCbCr conversion.  The stream ignores unchanged values.
 */
static void t6_crtc_update_color(struct t6_head *head,
				 const struct drm_crtc_state *crtc_state)
{
	const struct drm_color_ctm *ctm = NULL;
	const struct drm_color_lut *lut = NULL;

	if (crtc_state->ctm)
		ctm = crtc_state->ctm->data;
	if (crtc_state->gamma_lut)
		lut = crtc_state->gamma_lut->data;
	t6_stream_set_color(head, ctm ? ctm->matrix : NULL,
			    lut ? &lut->red : NULL);
}

static void t6_crtc_atomic_flush(struct drm_crtc *crtc,
				 struct drm_atomic_state *state)
{
	struct t6_head *head = container_of(crtc, struct t6_head, crtc);
	struct drm_pending_vblank_event *event = crtc->state->event;

	t6_crtc_update_color(head, crtc->state);

	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs t6_crtc_helper_funcs = {
	.mode_valid = t6_crtc_mode_valid,
	.atomic_check = t6_crtc_atomic_check,
	.atomic_enable = t6_crtc_atomic_enable,
	.atomic_disable = t6_crtc_atomic_disable,
	.atomic_flush = t6_crtc_atomic_flush,
};

static const struct drm_crtc_funcs t6_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = t6_crtc_enable_vblank,
	.disable_vblank = t6_crtc_disable_vblank,
};

/* --- Setup -------------------------------------------------------------- */

static const struct drm_mode_config_funcs t6_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Enable CRTCs before updating planes, so the stream is running (and sized)
 * when the first damage arrives.
 */
static const struct drm_mode_config_helper_funcs t6_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static void t6_connector_type(u8 link, int *connector, int *encoder)
{
	if (link & (T6_LINK_DVO | T6_LINK_LVDS)) {
		*connector = DRM_MODE_CONNECTOR_HDMIA;
		*encoder = DRM_MODE_ENCODER_TMDS;
	} else if (link & T6_LINK_DVI) {
		*connector = DRM_MODE_CONNECTOR_DVID;
		*encoder = DRM_MODE_ENCODER_TMDS;
	} else {
		*connector = DRM_MODE_CONNECTOR_VGA;
		*encoder = DRM_MODE_ENCODER_DAC;
	}
}

static int t6_head_init(struct t6_head *head)
{
	struct drm_device *drm = &head->t6->drm;
	int connector_type, encoder_type, ret;
	struct drm_plane *cursor = NULL;

	t6_hrtimer_setup(&head->vblank_timer, t6_vblank_timer);
	head->vblank_period = ms_to_ktime(16);

	ret = drm_universal_plane_init(drm, &head->primary, 0,
				       &t6_primary_funcs, t6_primary_formats,
				       ARRAY_SIZE(t6_primary_formats),
				       t6_modifiers, DRM_PLANE_TYPE_PRIMARY,
				       NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&head->primary, &t6_primary_helper_funcs);
	drm_plane_enable_fb_damage_clips(&head->primary);

	if (t6_param_hw_cursor) {
		ret = t6_cursor_plane_init(head);
		if (ret)
			return ret;
		cursor = &head->cursor;
	}

	ret = drm_crtc_init_with_planes(drm, &head->crtc, &head->primary,
					cursor, &t6_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&head->crtc, &t6_crtc_helper_funcs);
	/*
	 * CTM and gamma LUT, no degamma.  X's modesetting driver offers the
	 * CTM only on CRTCs that also have a gamma LUT.
	 */
	drm_crtc_enable_color_mgmt(&head->crtc, 0, true, T6_JPEG_LUT_SIZE);
	ret = drm_mode_crtc_set_gamma_size(&head->crtc, T6_JPEG_LUT_SIZE);
	if (ret)
		return ret;

	t6_connector_type(head->link, &connector_type, &encoder_type);
	ret = drm_simple_encoder_init(drm, &head->encoder, encoder_type);
	if (ret)
		return ret;
	head->encoder.possible_crtcs = drm_crtc_mask(&head->crtc);

	ret = drm_connector_init(drm, &head->connector, &t6_connector_funcs,
				 connector_type);
	if (ret)
		return ret;
	drm_connector_helper_add(&head->connector, &t6_connector_helper_funcs);
	/* HDMI reports hot-plug through the interrupt endpoint; VGA is polled. */
	head->connector.polled = head->link == T6_LINK_DAC ?
		DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT :
		DRM_CONNECTOR_POLL_HPD;

	return drm_connector_attach_encoder(&head->connector, &head->encoder);
}

int t6_kms_init(struct t6_device *t6)
{
	struct drm_device *drm = &t6->drm;
	unsigned int i;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	/* No lower bound: it applies to every framebuffer, cursors included. */
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 2048;	/* bounded by the 4 MiB NV12 slot */
	drm->mode_config.max_height = 2048;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow = 1;
	drm->mode_config.cursor_width = 64;
	drm->mode_config.cursor_height = 64;
	drm->mode_config.funcs = &t6_mode_config_funcs;
	drm->mode_config.helper_private = &t6_mode_config_helpers;

	for (i = 0; i < t6->num_heads; i++) {
		ret = t6_head_init(&t6->heads[i]);
		if (ret)
			return ret;
	}

	ret = drm_vblank_init(drm, t6->num_heads);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}
