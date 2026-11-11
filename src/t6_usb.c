// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * USB transport for the MCT Trigger 6.
 *
 *  - Vendor control requests on EP0 configure the chip (modes, cursor, ...).
 *  - Bulk OUT (EP 0x02) carries "objects": a 32-byte header naming a video
 *    RAM address and length, then the payload.  Payloads are split into
 *    T6_BULK_CHUNK-sized URBs that are all queued at once so the host
 *    controller never idles between them; raw frames, larger than the URB
 *    pool, are sent in several such waves.
 *  - Interrupt IN (EP 0x83) reports hot-plug and frame-done events.  It is
 *    kept permanently submitted, as MCT's drivers do.
 */
#include <linux/slab.h>
#include <linux/usb.h>

#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "t6_drv.h"

#define T6_CTRL_BUF_LEN		4096
#define T6_CTRL_TIMEOUT_MS	2000
#define T6_BULK_HDR_TIMEOUT_MS	1000
#define T6_BULK_TIMEOUT_MS	3000
#define T6_FENCE_TIMEOUT_MS	100

#define T6_CTRL_OUT (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define T6_CTRL_IN  (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

int t6_ctrl_out(struct t6_device *t6, u8 request, u16 value, u16 index,
		const void *data, u16 len)
{
	int ret;

	if (len > T6_CTRL_BUF_LEN)
		return -EINVAL;

	mutex_lock(&t6->ctrl_lock);
	if (len)
		memcpy(t6->ctrl_buf, data, len);
	ret = usb_control_msg(t6->udev, usb_sndctrlpipe(t6->udev, 0), request,
			      T6_CTRL_OUT, value, index,
			      len ? t6->ctrl_buf : NULL, len,
			      T6_CTRL_TIMEOUT_MS);
	mutex_unlock(&t6->ctrl_lock);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

/* Returns the number of bytes received, which may be short. */
int t6_ctrl_in(struct t6_device *t6, u8 request, u16 value, u16 index,
	       void *data, u16 len)
{
	int ret;

	if (len > T6_CTRL_BUF_LEN)
		return -EINVAL;

	mutex_lock(&t6->ctrl_lock);
	ret = usb_control_msg(t6->udev, usb_rcvctrlpipe(t6->udev, 0), request,
			      T6_CTRL_IN, value, index, t6->ctrl_buf, len,
			      T6_CTRL_TIMEOUT_MS);
	if (ret > 0)
		memcpy(data, t6->ctrl_buf, ret);
	mutex_unlock(&t6->ctrl_lock);
	return ret;
}

int t6_ctrl_in_exact(struct t6_device *t6, u8 request, u16 value, u16 index,
		     void *data, u16 len)
{
	int ret = t6_ctrl_in(t6, request, value, index, data, len);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EPROTO;
}

static unsigned int t6_bulk_pipe(const struct t6_device *t6)
{
	return usb_sndbulkpipe(t6->udev, usb_endpoint_num(t6->ep_bulk_out));
}

static void t6_bulk_complete(struct urb *urb)
{
	/* Completion is collected through the anchor. */
}

/*
 * After a failed transfer the chip's bulk parser may be part-way through an
 * object; MCT's driver resynchronises it with this request.
 */
static void t6_bulk_recover(struct t6_device *t6, int err)
{
	unsigned int pipe = t6_bulk_pipe(t6);

	if (err == -ENODEV || err == -ESHUTDOWN)
		return;
	if (err == -EPIPE)
		usb_clear_halt(t6->udev, pipe);
	t6_ctrl_out(t6, T6_REQ_CANCEL_BULK_OUT, 0, 0, NULL, 0);
}

int t6_send_object(struct t6_device *t6, u32 addr, const u8 *buf, u32 len)
{
	unsigned int pipe = t6_bulk_pipe(t6);
	unsigned int submitted, i;
	u32 off = 0;
	int actual, ret;

	if (!len || len > T6_RAW_MAX_OBJECT)
		return -E2BIG;

	mutex_lock(&t6->bulk_lock);

	t6_bulk_hdr_encode(len, addr, t6->bulk_hdr);
	ret = usb_bulk_msg(t6->udev, pipe, t6->bulk_hdr, T6_BULK_HDR_LEN,
			   &actual, T6_BULK_HDR_TIMEOUT_MS);
	if (!ret && actual != T6_BULK_HDR_LEN)
		ret = -EIO;

	/*
	 * One header, then the payload.  Objects larger than the URB pool
	 * (raw frames) go in several waves through the same URBs.
	 */
	while (!ret && off < len) {
		submitted = 0;
		for (i = 0; i < T6_BULK_URBS && off < len; i++) {
			u32 size = min_t(u32, T6_BULK_CHUNK, len - off);
			struct urb *urb = t6->bulk_urb[i];

			memcpy(t6->bulk_chunk[i], buf + off, size);
			usb_fill_bulk_urb(urb, t6->udev, pipe,
					  t6->bulk_chunk[i], size,
					  t6_bulk_complete, t6);
			usb_anchor_urb(urb, &t6->bulk_anchor);
			ret = usb_submit_urb(urb, GFP_KERNEL);
			if (ret) {
				usb_unanchor_urb(urb);
				break;
			}
			submitted++;
			off += size;
		}

		if (!usb_wait_anchor_empty_timeout(&t6->bulk_anchor,
						   T6_BULK_TIMEOUT_MS)) {
			usb_kill_anchored_urbs(&t6->bulk_anchor);
			if (!ret)
				ret = -ETIMEDOUT;
		}

		for (i = 0; i < submitted && !ret; i++) {
			struct urb *urb = t6->bulk_urb[i];

			if (urb->status)
				ret = urb->status;
			else if (urb->actual_length !=
				 urb->transfer_buffer_length)
				ret = -EIO;
		}
	}

	if (ret)
		t6_bulk_recover(t6, ret);
	mutex_unlock(&t6->bulk_lock);
	return ret;
}

/* --- Interrupt endpoint ------------------------------------------------- */

static void t6_int_complete(struct urb *urb)
{
	struct t6_device *t6 = urb->context;
	struct t6_int_event ev;
	int ret;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;		/* killed or unplugged */
	default:
		goto resubmit;
	}

	if (t6_int_parse(urb->transfer_buffer, urb->actual_length, &ev) ||
	    !(ev.func_mask & T6_INT_FUNC_DISPLAY))
		goto resubmit;

	if (ev.display_event & (T6_INT_DISP_HOTPLUG0 | T6_INT_DISP_HOTPLUG1))
		schedule_work(&t6->hotplug_work);
	if (ev.display_event & T6_INT_DISP_FRAME_DONE) {
		atomic_set(&t6->fence_done, ev.fence);
		WRITE_ONCE(t6->fences_seen, true);
		wake_up(&t6->fence_wq);
	}
	if (ev.display_event & T6_INT_DISP_JPEG_ERROR)
		dev_warn_ratelimited(t6->drm.dev, "JPEG decoder error (fence %u)\n",
				     ev.fence);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -ENODEV && ret != -EPERM)
		drm_err_ratelimited(&t6->drm, "interrupt resubmit failed: %d\n",
				    ret);
}

static void t6_hotplug_work(struct work_struct *work)
{
	struct t6_device *t6 = container_of(work, struct t6_device,
					    hotplug_work);

	drm_helper_hpd_irq_event(&t6->drm);
}

int t6_int_start(struct t6_device *t6)
{
	return usb_submit_urb(t6->int_urb, GFP_KERNEL);
}

void t6_int_stop(struct t6_device *t6)
{
	usb_kill_urb(t6->int_urb);
	cancel_work_sync(&t6->hotplug_work);
}

/*
 * Wait until fewer than @max_inflight video objects are still being
 * processed by the chip.  Only used once the chip has shown that it reports
 * frame completion; otherwise frames are simply sent as they are produced.
 */
int t6_wait_fences(struct t6_device *t6, unsigned int max_inflight)
{
	long ret;

	if (!READ_ONCE(t6->fences_seen))
		return 0;

	ret = wait_event_timeout(t6->fence_wq,
				 (u32)(atomic_read(&t6->fence_sent) -
				       atomic_read(&t6->fence_done)) < max_inflight,
				 msecs_to_jiffies(T6_FENCE_TIMEOUT_MS));
	return ret ? 0 : -ETIMEDOUT;
}

/* --- Setup -------------------------------------------------------------- */

int t6_usb_init(struct t6_device *t6)
{
	const struct usb_endpoint_descriptor *int_ep = t6->ep_int_in;
	unsigned int i;

	mutex_init(&t6->ctrl_lock);
	mutex_init(&t6->bulk_lock);
	init_usb_anchor(&t6->bulk_anchor);
	init_waitqueue_head(&t6->fence_wq);
	INIT_WORK(&t6->hotplug_work, t6_hotplug_work);

	t6->ctrl_buf = kmalloc(T6_CTRL_BUF_LEN, GFP_KERNEL);
	t6->bulk_hdr = kmalloc(T6_BULK_HDR_LEN, GFP_KERNEL);
	t6->int_buf = kmalloc(T6_INT_PKT_LEN, GFP_KERNEL);
	t6->int_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!t6->ctrl_buf || !t6->bulk_hdr || !t6->int_buf || !t6->int_urb)
		goto nomem;

	for (i = 0; i < T6_BULK_URBS; i++) {
		t6->bulk_chunk[i] = kmalloc(T6_BULK_CHUNK, GFP_KERNEL);
		t6->bulk_urb[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!t6->bulk_chunk[i] || !t6->bulk_urb[i])
			goto nomem;
	}

	usb_fill_int_urb(t6->int_urb, t6->udev,
			 usb_rcvintpipe(t6->udev, usb_endpoint_num(int_ep)), t6->int_buf,
			 T6_INT_PKT_LEN, t6_int_complete, t6,
			 int_ep->bInterval);
	return 0;

nomem:
	t6_usb_fini(t6);
	return -ENOMEM;
}

void t6_usb_fini(struct t6_device *t6)
{
	unsigned int i;

	for (i = 0; i < T6_BULK_URBS; i++) {
		usb_free_urb(t6->bulk_urb[i]);
		kfree(t6->bulk_chunk[i]);
		t6->bulk_urb[i] = NULL;
		t6->bulk_chunk[i] = NULL;
	}
	usb_free_urb(t6->int_urb);
	kfree(t6->int_buf);
	kfree(t6->bulk_hdr);
	kfree(t6->ctrl_buf);
	t6->int_urb = NULL;
	t6->int_buf = NULL;
	t6->bulk_hdr = NULL;
	t6->ctrl_buf = NULL;
}
