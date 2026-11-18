// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * DRM driver for USB display adapters and docks built on the Magic Control
 * Technology (MCT) Trigger 6 chip, e.g. j5create docks.
 *
 * The chip appears as one vendor-specific USB device ("T6 USB Station",
 * 0711:5600-561f / 5650-565f) with up to two video outputs.  See
 * docs/PROTOCOL.md for the protocol and docs/DESIGN.md for the design.
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "t6_compat.h"
#include "t6_drv.h"

unsigned int t6_param_quality = 85;
module_param_named(jpeg_quality, t6_param_quality, uint, 0644);
MODULE_PARM_DESC(jpeg_quality,
		 "Maximum JPEG quality, 30-100 (default 85); lowered automatically when the link is saturated");

unsigned int t6_param_encode_jobs;
module_param_named(encode_jobs, t6_param_encode_jobs, uint, 0644);
MODULE_PARM_DESC(encode_jobs,
		 "Parallel JPEG encoders per frame (default 0 = one per CPU, max 8)");

bool t6_param_hw_cursor = true;
module_param_named(hw_cursor, t6_param_hw_cursor, bool, 0444);
MODULE_PARM_DESC(hw_cursor, "Use the chip's hardware cursor (default true)");

int t6_param_lossless = -1;
module_param_named(lossless, t6_param_lossless, int, 0644);
MODULE_PARM_DESC(lossless,
		 "Send a lossless RGB frame once the screen settles: 1 = on any link, 0 = never (JPEG only), -1 = on SuperSpeed (USB 3) links (default -1)");

unsigned int t6_param_lossless_delay_ms = 100;
module_param_named(lossless_delay_ms, t6_param_lossless_delay_ms, uint, 0644);
MODULE_PARM_DESC(lossless_delay_ms,
		 "Quiet time before the lossless frame; updates closer together than this are sent as JPEG (default 100)");

bool t6_param_lossless_always;
module_param_named(lossless_always, t6_param_lossless_always, bool, 0644);
MODULE_PARM_DESC(lossless_always,
		 "When lossless is in effect, send every frame lossless, never JPEG, however slow (default false; lossless_min_fps already keeps motion lossless while the link allows)");

unsigned int t6_param_lossless_min_fps = 20;
module_param_named(lossless_min_fps, t6_param_lossless_min_fps, uint, 0644);
MODULE_PARM_DESC(lossless_min_fps,
		 "When lossless is in effect, moving content is sent lossless too while lossless frames still reach this rate; below it, motion is JPEG and the still picture is refined. 0 = motion always JPEG (default 20)");

int t6_param_trust_damage = -1;
module_param_named(trust_damage, t6_param_trust_damage, int, 0644);
MODULE_PARM_DESC(trust_damage,
		 "Update only userspace's damage rectangles: 1 = always, 0 = never (compare whole frames), -1 = until a rectangle falls outside the framebuffer, as with X11 and a rotated output (default -1)");

/* --- Device discovery --------------------------------------------------- */

static int t6_read_timings(struct t6_head *head, unsigned int fallback_count)
{
	struct t6_device *t6 = head->t6;
	unsigned int count, i, off, len;
	__le32 count_le;
	u8 *raw;
	int ret;

	ret = t6_ctrl_in_exact(t6, T6_REQ_GET_TIMING_COUNT, head->index, 0,
			       &count_le, sizeof(count_le));
	count = ret ? fallback_count : le32_to_cpu(count_le);
	if (!count || count > T6_TIMING_MAX_ENTRIES) {
		drm_err(&t6->drm, "head %u: bad timing table size %u\n",
			head->index, count);
		return -EPROTO;
	}

	len = count * T6_TIMING_LEN;
	raw = kmalloc(len, GFP_KERNEL);
	head->timings = drmm_kcalloc(&t6->drm, count, sizeof(*head->timings),
				     GFP_KERNEL);
	if (!raw || !head->timings) {
		kfree(raw);
		return -ENOMEM;
	}

	/* wValue = head, wIndex = byte offset, as the Windows driver does. */
	for (off = 0; off < len; off += T6_TIMING_TABLE_CHUNK) {
		u16 chunk = min_t(unsigned int, T6_TIMING_TABLE_CHUNK, len - off);

		ret = t6_ctrl_in_exact(t6, T6_REQ_GET_TIMING_TABLE, head->index,
				       off, raw + off, chunk);
		if (ret)
			goto out;
	}

	head->num_timings = 0;
	for (i = 0; i < count; i++) {
		struct t6_timing *t = &head->timings[head->num_timings];

		t6_timing_decode(raw + i * T6_TIMING_LEN, t);
		if (t6_timing_sane(t))
			head->num_timings++;
		else
			drm_dbg_driver(&t6->drm, "head %u: ignoring timing %u\n",
				       head->index, i);
	}
	ret = head->num_timings ? 0 : -EPROTO;
out:
	kfree(raw);
	return ret;
}

static int t6_discover(struct t6_device *t6)
{
	struct t6_section section;
	unsigned int i, primary = 0;
	__le32 version;
	u8 vram, *buf;
	int ret;

	if (!t6_ctrl_in_exact(t6, T6_REQ_GET_VERSION, 0, T6_VERSION_HW_PLATFORM,
			      &version, sizeof(version)))
		t6->hw_platform = le32_to_cpu(version);
	if (!t6_ctrl_in_exact(t6, T6_REQ_GET_VERSION, 0, T6_VERSION_IMAGE_CODE,
			      &version, sizeof(version)))
		t6->fw_version = le32_to_cpu(version);

	ret = t6_ctrl_in_exact(t6, T6_REQ_GET_VRAM_SIZE, 0, 0, &vram, 1);
	if (ret)
		return ret;
	t6->vram_mb = vram;

	buf = kmalloc(T6_SECTION_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = t6_ctrl_in_exact(t6, T6_REQ_QUERY_SECTION, 0, 0, buf,
			       T6_SECTION_LEN);
	if (!ret)
		ret = t6_section_parse(buf, T6_SECTION_LEN, &section);
	kfree(buf);
	if (ret)
		return ret;

	/* Heads are numbered by protocol output index; skip absent ones. */
	for (i = 0; i < T6_MAX_HEADS; i++) {
		struct t6_head *head;

		if (!section.link[i])
			continue;
		head = &t6->heads[t6->num_heads++];
		head->t6 = t6;
		head->index = i;
		head->link = section.link[i];
		ret = t6_read_timings(head, section.num_timings[i]);
		if (ret)
			return ret;
	}
	if (!t6->num_heads)
		return -ENODEV;

	/* With two heads, the HDMI-capable one gets the larger VRAM share. */
	for (i = 0; i < t6->num_heads; i++)
		if (t6->heads[i].link & T6_LINK_DVO) {
			primary = i;
			break;
		}
	for (i = 0; i < t6->num_heads; i++) {
		ret = t6_vram_layout(t6->vram_mb, t6->num_heads, i == primary,
				     &t6->heads[i].vram);
		if (ret) {
			drm_err(&t6->drm, "unsupported video RAM size %u MiB\n",
				t6->vram_mb);
			return ret;
		}
	}

	drm_info(&t6->drm,
		 "Trigger 6: %u output(s), %u MiB VRAM, platform %u, firmware %#x, %s\n",
		 t6->num_heads, t6->vram_mb, t6->hw_platform, t6->fw_version,
		 usb_speed_string(t6->udev->speed));
	return 0;
}

/* --- debugfs ------------------------------------------------------------ */

static int t6_stats_show(struct seq_file *m, void *unused)
{
	struct t6_device *t6 = m->private;
	unsigned int i;

	seq_printf(m, "link: %s\nfences: sent %u done %u (%s)\n",
		   usb_speed_string(t6->udev->speed),
		   atomic_read(&t6->fence_sent), atomic_read(&t6->fence_done),
		   READ_ONCE(t6->fences_seen) ? "reported" : "not reported");
	for (i = 0; i < t6->num_heads; i++) {
		const struct t6_stream *s = &t6->heads[i].stream;
		const struct t6_stats *st = &s->stats;

		seq_printf(m,
			   "head %u: %ux%u quality %u lossless %s motion %s raw_cost_ms %llu frames %llu raw_frames %llu bytes %llu last %u rows %llu dropped %llu fence_timeouts %llu encode_ms %llu send_ms %llu\n",
			   t6->heads[i].index, s->width, s->height, s->quality,
			   s->raw_ok ? "yes" : "no",
			   s->raw_ok && s->raw_motion ? "raw" : "jpeg",
			   div_u64(s->raw_cost_ns, NSEC_PER_MSEC),
			   st->frames, st->raw_frames, st->bytes,
			   st->last_frame_bytes,
			   st->rows_encoded, st->dropped, st->fence_timeouts,
			   div_u64(st->encode_ns, NSEC_PER_MSEC),
			   div_u64(st->send_ns, NSEC_PER_MSEC));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(t6_stats);

/* /sys/kernel/debug/mct_t6-<usb interface>/stats */
static void t6_debugfs_init(struct t6_device *t6)
{
	char name[64];

	snprintf(name, sizeof(name), DRIVER_NAME "-%s", dev_name(&t6->intf->dev));
	t6->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("stats", 0444, t6->debugfs, t6, &t6_stats_fops);
}

/* --- DRM driver --------------------------------------------------------- */

DEFINE_DRM_GEM_FOPS(t6_fops);

#ifndef T6_HAVE_DRM_DMA_DEV
static struct drm_gem_object *t6_gem_prime_import(struct drm_device *drm,
						  struct dma_buf *dma_buf)
{
	struct t6_device *t6 = to_t6(drm);

	if (!t6->dmadev)
		return ERR_PTR(-ENODEV);
	return drm_gem_prime_import_dev(drm, dma_buf, t6->dmadev);
}
#endif

static const struct drm_driver t6_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &t6_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
#ifndef T6_HAVE_DRM_DMA_DEV
	.gem_prime_import = t6_gem_prime_import,
#endif
	T6_FBDEV_DRIVER_OPS
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	T6_DRIVER_DATE
	.major = 1,
	.minor = 0,
};

/* --- USB driver --------------------------------------------------------- */

static void t6_teardown(struct t6_device *t6)
{
	unsigned int i;

	t6_int_stop(t6);
	for (i = 0; i < T6_MAX_HEADS; i++)
		t6_stream_fini(&t6->heads[i]);
	if (t6->wq)
		destroy_workqueue(t6->wq);
	t6->wq = NULL;
	t6_usb_fini(t6);
	put_device(t6->dmadev);
	t6->dmadev = NULL;
}

static int t6_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct t6_device *t6;
	struct drm_device *drm;
	unsigned int i;
	int ret;

	t6 = devm_drm_dev_alloc(&intf->dev, &t6_drm_driver, struct t6_device,
				drm);
	if (IS_ERR(t6))
		return PTR_ERR(t6);
	drm = &t6->drm;
	t6->intf = intf;
	t6->udev = interface_to_usbdev(intf);
	usb_set_intfdata(intf, t6);

	/*
	 * Real chips use bulk OUT 0x02 and interrupt IN 0x83 (plus an unused
	 * bulk IN); look them up by type like other USB drivers do.
	 */
	ret = usb_find_common_endpoints(intf->cur_altsetting, NULL,
					&t6->ep_bulk_out, &t6->ep_int_in, NULL);
	if (ret)
		return ret;
	for (i = 0; i < T6_MAX_HEADS; i++) {
		t6->heads[i].t6 = t6;
		t6_stream_init(&t6->heads[i]);
	}

	t6->dmadev = usb_intf_get_dma_device(intf);
	if (!t6->dmadev)
		drm_warn(drm, "no DMA device; buffer sharing (PRIME) disabled\n");
#ifdef T6_HAVE_DRM_DMA_DEV
	else
		drm_dev_set_dma_dev(drm, t6->dmadev);
#endif

	ret = t6_usb_init(t6);
	if (ret)
		goto err;

	ret = t6_discover(t6);
	if (ret) {
		drm_err(drm, "device discovery failed: %d\n", ret);
		goto err;
	}

	t6->wq = alloc_workqueue(DRIVER_NAME, WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!t6->wq) {
		ret = -ENOMEM;
		goto err;
	}
	ret = t6_kms_init(t6);
	if (ret)
		goto err;

	ret = t6_int_start(t6);
	if (ret)
		goto err;

	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_poll;

	t6_debugfs_init(t6);

	t6_fbdev_setup(drm);
	return 0;

err_poll:
	drm_kms_helper_poll_fini(drm);
err:
	t6_teardown(t6);
	return ret;
}

static void t6_disconnect(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	struct drm_device *drm = &t6->drm;
	unsigned int i;

	debugfs_remove_recursive(t6->debugfs);
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	for (i = 0; i < t6->num_heads; i++)
		hrtimer_cancel(&t6->heads[i].vblank_timer);
	t6_teardown(t6);
}

static int t6_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	int ret;

	ret = drm_mode_config_helper_suspend(&t6->drm);
	if (ret)
		return ret;
	t6_int_stop(t6);
	return 0;
}

static int t6_resume(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);
	int ret;

	ret = t6_int_start(t6);
	if (ret)
		drm_err(&t6->drm, "interrupt endpoint restart failed: %d\n", ret);
	/* Re-enabling the CRTCs re-sends the mode, which re-initialises heads. */
	return drm_mode_config_helper_resume(&t6->drm);
}

#ifdef T6_HAVE_USB_SHUTDOWN
static void t6_shutdown(struct usb_interface *intf)
{
	struct t6_device *t6 = usb_get_intfdata(intf);

	drm_atomic_helper_shutdown(&t6->drm);
}
#endif

/* MCT's Windows bus driver (t6sta.inf) binds exactly these product IDs. */
#define T6_ID(pid) { USB_DEVICE_AND_INTERFACE_INFO(T6_USB_VID, pid, \
						   USB_CLASS_VENDOR_SPEC, 0, 0) }

static const struct usb_device_id t6_id_table[] = {
	T6_ID(0x5600), T6_ID(0x5601), T6_ID(0x5602), T6_ID(0x5603),
	T6_ID(0x5604), T6_ID(0x5605), T6_ID(0x5606), T6_ID(0x5607),
	T6_ID(0x5608), T6_ID(0x5609), T6_ID(0x560a), T6_ID(0x560b),
	T6_ID(0x560c), T6_ID(0x560d), T6_ID(0x560e), T6_ID(0x560f),
	T6_ID(0x5610), T6_ID(0x5611), T6_ID(0x5612), T6_ID(0x5613),
	T6_ID(0x5614), T6_ID(0x5615), T6_ID(0x5616), T6_ID(0x5617),
	T6_ID(0x5618), T6_ID(0x5619), T6_ID(0x561a), T6_ID(0x561b),
	T6_ID(0x561c), T6_ID(0x561d), T6_ID(0x561e), T6_ID(0x561f),
	T6_ID(0x5650), T6_ID(0x5651), T6_ID(0x5652), T6_ID(0x5653),
	T6_ID(0x5654), T6_ID(0x5655), T6_ID(0x5656), T6_ID(0x5657),
	T6_ID(0x5658), T6_ID(0x5659), T6_ID(0x565a), T6_ID(0x565b),
	T6_ID(0x565c), T6_ID(0x565d), T6_ID(0x565e), T6_ID(0x565f),
	{ }
};
MODULE_DEVICE_TABLE(usb, t6_id_table);

static struct usb_driver t6_usb_driver = {
	.name = DRIVER_NAME,
	.probe = t6_probe,
	.disconnect = t6_disconnect,
	.suspend = t6_suspend,
	.resume = t6_resume,
	.reset_resume = t6_resume,
#ifdef T6_HAVE_USB_SHUTDOWN
	.shutdown = t6_shutdown,
#endif
	.id_table = t6_id_table,
};

module_usb_driver(t6_usb_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Gareth Palmer");
MODULE_LICENSE("GPL");
