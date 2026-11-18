// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * t6emu: software MCT Trigger 6 for end-to-end testing of mct_t6.
 *
 * Runs inside the test kernel (UML) and binds to dummy_udc through
 * raw-gadget, so the dummy host controller sees a "T6 USB Station"
 * (0711:5601, High Speed) and mct_t6 probes it like real hardware.
 *
 *  - EP0: standard enumeration plus the vendor requests of
 *    docs/PROTOCOL.md, answered with data captured from a real dock
 *    (t6emu_data.h).  OUT requests are accepted and logged; requests the
 *    driver must never send (ROM writes, reset, audio, gamma) are stalled
 *    and flagged.
 *  - Bulk OUT: every object is parsed and validated against the protocol
 *    (header, ring placement, 48-byte video header, JPEG framing), and
 *    each JPEG is written to <outdir>/frame-NNNNN.jpg.
 *  - Interrupt IN: a "frame done" packet echoing the fence follows every
 *    video object, unless --no-fence is given.
 *
 * Every event is appended to <outdir>/log.jsonl, one JSON object per line.
 * <outdir>/emu.status is rewritten after each frame so that test clients
 * can wait for frames without parsing the log.
 *
 * EDID policy: a head whose connector is "connected" serves the 1080p
 * monitor EDID; a disconnected head has no monitor, so the 0x80 request is
 * stalled, like a DDC read that nobody answers.
 *
 * Threads: the main thread runs the EP0 event loop, one thread reads bulk
 * OUT and one writes interrupt IN.  SIGTERM/SIGINT stop all three: the
 * signal interrupts the blocking raw-gadget ioctls (installed without
 * SA_RESTART) and the workers are kicked with SIGUSR1.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>

#include "t6emu_data.h"

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ALIGN(x, a)		(((x) + (a) - 1) / (a) * (a))
#define MiB			(1024u * 1024u)

/* --- Device identity --------------------------------------------------- */

#define T6_VID			0x0711
#define T6_PID			0x5601
#define T6_VRAM_MB		58
#define T6_NUM_HEADS		2

/* Version words (0xB0, wIndex = field) as the captured dock reports them. */
static const uint32_t t6_versions[] = {
	[0] = 1,		/* hardware platform: 1 = "Super Lite" */
	[1] = 0x160628b9,	/* boot code */
	[2] = 0x21081301,	/* image code */
	[4] = 8,
};

#define BULK_MAXP		512
#define INT_MAXP		64
#define INT_INTERVAL		4	/* 2^(4-1) microframes = 1 ms */

/* --- Protocol constants (docs/PROTOCOL.md) ---------------------------- */

#define BULK_HDR_LEN		32
#define VIDEO_HDR_LEN		48
#define JPEG_PAD		1024
#define FB_SLOTS		3
#define FB_SLOT_SIZE		(4 * MiB)
#define RESET_FRAMES		10	/* frames after a mode set with 0x80 */
#define CURSOR_HDR_LEN		8
#define CURSOR_MAX_BYTES	(CURSOR_HDR_LEN + 128 * 128 * 4)
#define CURSOR_MAX_SLOT		9	/* shape slots 0-9 per head (MCT t6.h) */
#define MAX_OBJECT		(16 * MiB)	/* sanity bound, > any ring */

enum {
	REQ_MONITOR_CTRL	= 0x03,
	REQ_CURSOR1_POS		= 0x04,
	REQ_CURSOR1_STATE	= 0x05,
	REQ_CURSOR2_POS		= 0x06,
	REQ_CURSOR2_STATE	= 0x07,
	REQ_CURSOR1_SHAPE	= 0x10,
	REQ_CURSOR2_SHAPE	= 0x11,
	REQ_SET_TIMING		= 0x12,
	REQ_CANCEL_BULK		= 0x14,
	REQ_SOFTWARE_READY	= 0x31,
	REQ_GET_EDID		= 0x80,
	REQ_TIMING_COUNT	= 0x84,
	REQ_CONN_STATUS		= 0x87,
	REQ_VRAM_SIZE		= 0x88,
	REQ_TIMING_TABLE	= 0x89,
	REQ_VERSION		= 0xb0,
	REQ_SECTION		= 0xb3,
};

/* --- State ------------------------------------------------------------- */

struct head_state {
	bool exists;
	bool primary;		/* gets the larger VRAM share */
	bool connected;
	const uint8_t (*table)[32];
	unsigned int ntimings;

	/* VRAM layout, as MCT's reference driver lays it out. */
	uint32_t ring_base, ring_end, fb[FB_SLOTS];

	/* Mode set progress: 0x12, then 0x31, then 0x03 on. */
	bool timing_set, sw_ready, output_on;
	unsigned int width, height;
	unsigned int frames;	/* JPEG frames since the last 0x12 */

	/*
	 * Raw RGB24 frames (PROTOCOL.md 3.3): the span of VRAM they have used
	 * since the last mode set, which JPEG ring objects must avoid, and
	 * the surface the last flip put on screen, which no raw frame may
	 * overwrite.
	 */
	uint32_t raw_lo, raw_hi;
	uint32_t shown;

	uint8_t cursor[CURSOR_MAX_BYTES];
};

static struct {
	const char *outdir;
	bool no_fence;
	int fd;			/* /dev/raw-gadget */
	FILE *log;
	pthread_mutex_t lock;	/* log, counters and head state */
	struct timespec t0;
	unsigned long seq;

	/* Chosen endpoints: address and raw-gadget handle. */
	uint8_t ep_bulk_in, ep_bulk_out, ep_int_in;
	int h_bulk_out, h_int_in;
	bool configured;
	pthread_t bulk_thread, int_thread;

	struct head_state heads[T6_NUM_HEADS];

	/* Frame accounting. */
	unsigned long frames, raw_frames, objects, bytes, errors;
	uint32_t last_fence;
	bool any_fence;

	/* Interrupt IN queue: fences waiting to be reported. */
	pthread_cond_t int_cond;
	uint32_t int_queue[64];
	unsigned int int_head, int_tail;
} emu = {
	.fd = -1,
	.h_bulk_out = -1,
	.h_int_in = -1,
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.int_cond = PTHREAD_COND_INITIALIZER,
};

static volatile sig_atomic_t stopping;

/* --- Helpers ----------------------------------------------------------- */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "t6emu: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static uint16_t get_le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t get_le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static double elapsed_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - emu.t0.tv_sec) * 1e3 +
	       (t.tv_nsec - emu.t0.tv_nsec) / 1e6;
}

/*
 * A small growable string for building one JSON line.  Validation errors
 * are collected in a second one and spliced in as an array.
 */
struct sbuf {
	char *s;
	size_t len, cap;
};

static void sb_printf(struct sbuf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
		va_end(ap);
		if (n >= 0 && b->len + n < b->cap)
			break;
		b->cap = (b->cap + n + 1) * 2;
		b->s = realloc(b->s, b->cap);
		if (!b->s)
			die("out of memory");
	}
	b->len += n;
}

static void sb_hex(struct sbuf *b, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		sb_printf(b, "%02x", p[i]);
}

/* Record a validation error (fmt must not contain JSON metacharacters). */
static void err_add(struct sbuf *errs, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	sb_printf(errs, "%s\"%s\"", errs->len ? "," : "", msg);
}

/* Append one event to log.jsonl.  Caller holds emu.lock. */
static void log_event(struct sbuf *body, struct sbuf *errs)
{
	fprintf(emu.log, "{\"seq\":%lu,\"t_ms\":%.3f,%s,\"errors\":[%s]}\n",
		emu.seq++, elapsed_ms(), body->s ? body->s : "",
		errs && errs->s ? errs->s : "");
	fflush(emu.log);
	if (errs && errs->len) {
		emu.errors++;
		fprintf(stderr, "t6emu: protocol error: %s\n", errs->s);
	}
}

static void log_simple(const char *event, const char *fmt, ...)
{
	struct sbuf b = { 0 };
	char extra[256] = "";
	va_list ap;

	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(extra, sizeof(extra), fmt, ap);
		va_end(ap);
	}
	sb_printf(&b, "\"ev\":\"%s\"%s%s", event, *extra ? "," : "", extra);
	pthread_mutex_lock(&emu.lock);
	log_event(&b, NULL);
	pthread_mutex_unlock(&emu.lock);
	free(b.s);
}

/* Rewrite <outdir>/emu.status atomically.  Caller holds emu.lock. */
static void write_status(void)
{
	char tmp[4096], path[4096];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s/emu.status.tmp", emu.outdir);
	snprintf(path, sizeof(path), "%s/emu.status", emu.outdir);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "frames %lu\nraw_frames %lu\nobjects %lu\nbytes %lu\nerrors %lu\nlast_fence %u\n",
		emu.frames, emu.raw_frames, emu.objects, emu.bytes, emu.errors,
		emu.last_fence);
	fclose(f);
	rename(tmp, path);
}

/* --- Device model ------------------------------------------------------ */

/*
 * VRAM layout per head, following MCT's reference driver for 1080p JPEG
 * streaming (docs/PROTOCOL.md section 3.2): with one head the ring is
 * [0, top-12M) and the slots are at top-12M/-8M/-4M; with two heads the
 * primary head gets [0, top-30M) with slots below the other head's ring.
 */
static void head_layout(struct head_state *h, unsigned int nheads)
{
	unsigned int top = T6_VRAM_MB, i;

	if (nheads == 1) {
		h->ring_base = 0;
	} else if (h->primary) {
		h->ring_base = 0;
		top = T6_VRAM_MB - 18;
	} else {
		h->ring_base = (T6_VRAM_MB - 18) * MiB;
	}
	for (i = 0; i < FB_SLOTS; i++)
		h->fb[i] = (top - 12 + 4 * i) * MiB;
	h->ring_end = h->fb[0];
}

static void model_init(bool conn0, bool conn1)
{
	unsigned int i, nheads = 0;
	int primary = -1;

	emu.heads[0].table = t6_timings_head0;
	emu.heads[0].ntimings = ARRAY_SIZE(t6_timings_head0);
	emu.heads[1].table = t6_timings_head1;
	emu.heads[1].ntimings = ARRAY_SIZE(t6_timings_head1);
	emu.heads[0].connected = conn0;
	emu.heads[1].connected = conn1;

	/* Heads and the primary (HDMI-capable) one come from the caps words. */
	for (i = 0; i < T6_NUM_HEADS; i++) {
		uint32_t caps = get_le32(t6_section_hdr + 0x58 + 4 * i);

		emu.heads[i].exists = caps & 0xf;
		if (!emu.heads[i].exists)
			continue;
		nheads++;
		if (primary < 0 && (caps & 0x2))
			primary = i;
		if (((caps >> 8) & 0xff) != emu.heads[i].ntimings)
			die("section header and timing table disagree");
	}
	if (primary < 0)
		primary = 0;
	for (i = 0; i < T6_NUM_HEADS; i++) {
		emu.heads[i].primary = (int)i == primary;
		if (emu.heads[i].exists)
			head_layout(&emu.heads[i], nheads);
	}
}

/* --- Descriptors ------------------------------------------------------- */

static const struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_VENDOR_SPEC,
	.bMaxPacketSize0 = 64,
	.idVendor = T6_VID,
	.idProduct = T6_PID,
	.bcdDevice = 0x0110,
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

static const struct usb_qualifier_descriptor qual_desc = {
	.bLength = sizeof(struct usb_qualifier_descriptor),
	.bDescriptorType = USB_DT_DEVICE_QUALIFIER,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_VENDOR_SPEC,
	.bMaxPacketSize0 = 64,
	.bNumConfigurations = 1,
};

static const char *const strings[] = { NULL, "MCT Corp.", "USB Station" };

static void make_ep_desc(struct usb_endpoint_descriptor *d, uint8_t addr,
			 uint8_t type, uint16_t maxp, uint8_t interval)
{
	memset(d, 0, sizeof(*d));
	d->bLength = USB_DT_ENDPOINT_SIZE;
	d->bDescriptorType = USB_DT_ENDPOINT;
	d->bEndpointAddress = addr;
	d->bmAttributes = type;
	d->wMaxPacketSize = maxp;
	d->bInterval = interval;
}

static void ep_descs(struct usb_endpoint_descriptor eps[3])
{
	make_ep_desc(&eps[0], emu.ep_bulk_in, USB_ENDPOINT_XFER_BULK,
		     BULK_MAXP, 0);
	make_ep_desc(&eps[1], emu.ep_bulk_out, USB_ENDPOINT_XFER_BULK,
		     BULK_MAXP, 0);
	make_ep_desc(&eps[2], emu.ep_int_in, USB_ENDPOINT_XFER_INT,
		     INT_MAXP, INT_INTERVAL);
}

/* Configuration descriptor with interface and endpoints; returns length. */
static size_t build_config(uint8_t *buf, uint8_t type)
{
	struct usb_config_descriptor cfg = {
		.bLength = USB_DT_CONFIG_SIZE,
		.bDescriptorType = type,
		.bNumInterfaces = 1,
		.bConfigurationValue = 1,
		.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
		.bMaxPower = 0x1f,
	};
	struct usb_interface_descriptor intf = {
		.bLength = USB_DT_INTERFACE_SIZE,
		.bDescriptorType = USB_DT_INTERFACE,
		.bNumEndpoints = 3,
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
	};
	struct usb_endpoint_descriptor eps[3];
	size_t len = 0, i;

	ep_descs(eps);
	memcpy(buf + len, &cfg, USB_DT_CONFIG_SIZE);
	len += USB_DT_CONFIG_SIZE;
	memcpy(buf + len, &intf, USB_DT_INTERFACE_SIZE);
	len += USB_DT_INTERFACE_SIZE;
	for (i = 0; i < 3; i++) {
		memcpy(buf + len, &eps[i], USB_DT_ENDPOINT_SIZE);
		len += USB_DT_ENDPOINT_SIZE;
	}
	put_le16(buf + 2, len);
	return len;
}

/*
 * Pick UDC endpoints for bulk IN, bulk OUT and interrupt IN.  Fixed-function
 * endpoints (dummy_udc's "ep1in-bulk" etc.) dictate the address; configurable
 * ones ("ep-a") get the lowest unused number.
 */
static void choose_endpoints(void)
{
	struct usb_raw_eps_info info;
	bool used[16] = { true };	/* ep0 */
	bool taken[USB_RAW_EPS_NUM_MAX] = { false };
	struct {
		const char *what;
		bool bulk, in;
		uint16_t maxp;
		uint8_t *addr;
	} want[] = {
		{ "bulk IN", true, true, BULK_MAXP, &emu.ep_bulk_in },
		{ "bulk OUT", true, false, BULK_MAXP, &emu.ep_bulk_out },
		{ "interrupt IN", false, true, INT_MAXP, &emu.ep_int_in },
	};
	int n, i, w;

	memset(&info, 0, sizeof(info));
	n = ioctl(emu.fd, USB_RAW_IOCTL_EPS_INFO, &info);
	if (n < 0)
		die("EPS_INFO: %s", strerror(errno));

	for (i = 0; i < n; i++)
		if (info.eps[i].addr != USB_RAW_EP_ADDR_ANY &&
		    info.eps[i].addr < 16)
			used[info.eps[i].addr] = true;

	for (w = 0; w < (int)ARRAY_SIZE(want); w++) {
		for (i = 0; i < n; i++) {
			struct usb_raw_ep_caps *c = &info.eps[i].caps;
			unsigned int num;

			if (taken[i] || (want[w].bulk ? !c->type_bulk : !c->type_int) ||
			    (want[w].in ? !c->dir_in : !c->dir_out) ||
			    info.eps[i].limits.maxpacket_limit < want[w].maxp)
				continue;
			num = info.eps[i].addr;
			if (num == USB_RAW_EP_ADDR_ANY) {
				for (num = 1; num < 16 && used[num]; num++)
					;
				if (num == 16)
					continue;
				used[num] = true;
			}
			taken[i] = true;
			*want[w].addr = num | (want[w].in ? USB_DIR_IN : 0);
			fprintf(stderr, "t6emu: %s -> %s (0x%02x)\n", want[w].what,
				info.eps[i].name, *want[w].addr);
			break;
		}
		if (i == n)
			die("UDC has no endpoint for %s", want[w].what);
	}
}

/* --- EP0 --------------------------------------------------------------- */

struct ep0_io {
	struct usb_raw_ep_io io;
	uint8_t data[4096];
};

static int ep0_write(const void *data, size_t len)
{
	struct ep0_io r;

	r.io.ep = 0;
	r.io.flags = 0;
	r.io.length = len;
	memcpy(r.data, data, len);
	return ioctl(emu.fd, USB_RAW_IOCTL_EP0_WRITE, &r);
}

static int ep0_read(void *data, size_t len)
{
	struct ep0_io r;
	int ret;

	r.io.ep = 0;
	r.io.flags = 0;
	r.io.length = len;
	ret = ioctl(emu.fd, USB_RAW_IOCTL_EP0_READ, &r);
	if (ret > 0)
		memcpy(data, r.data, ret);
	return ret;
}

static void ep0_stall(void)
{
	if (ioctl(emu.fd, USB_RAW_IOCTL_EP0_STALL, 0) < 0)
		fprintf(stderr, "t6emu: EP0_STALL: %s\n", strerror(errno));
}

static void *bulk_thread_fn(void *arg);
static void *int_thread_fn(void *arg);

static int enable_ep(uint8_t addr, uint8_t type, uint16_t maxp,
		     uint8_t interval)
{
	struct usb_endpoint_descriptor d;
	int h;

	make_ep_desc(&d, addr, type, maxp, interval);
	h = ioctl(emu.fd, USB_RAW_IOCTL_EP_ENABLE, &d);
	if (h < 0)
		die("EP_ENABLE 0x%02x: %s", addr, strerror(errno));
	return h;
}

/* SET_CONFIGURATION(1): enable endpoints, tell the UDC, start the workers. */
static void set_configuration(void)
{
	sigset_t block, old;

	if (emu.configured)
		return;
	enable_ep(emu.ep_bulk_in, USB_ENDPOINT_XFER_BULK, BULK_MAXP, 0);
	emu.h_bulk_out = enable_ep(emu.ep_bulk_out, USB_ENDPOINT_XFER_BULK,
				   BULK_MAXP, 0);
	emu.h_int_in = enable_ep(emu.ep_int_in, USB_ENDPOINT_XFER_INT,
				 INT_MAXP, INT_INTERVAL);
	if (ioctl(emu.fd, USB_RAW_IOCTL_VBUS_DRAW, 0x1f) < 0)
		fprintf(stderr, "t6emu: VBUS_DRAW: %s\n", strerror(errno));
	if (ioctl(emu.fd, USB_RAW_IOCTL_CONFIGURE, 0) < 0)
		die("CONFIGURE: %s", strerror(errno));
	emu.configured = true;

	/* Workers inherit a mask that leaves SIGTERM/SIGINT to this thread. */
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	if (pthread_create(&emu.bulk_thread, NULL, bulk_thread_fn, NULL) ||
	    pthread_create(&emu.int_thread, NULL, int_thread_fn, NULL))
		die("pthread_create failed");
	pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* Standard requests.  Returns the reply length, or -1 to stall. */
static int std_request(const struct usb_ctrlrequest *c, uint8_t *reply,
		       bool *handled_status)
{
	uint16_t value = c->wValue, i;
	unsigned int idx;
	const char *s;

	*handled_status = false;
	switch (c->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		if (!(c->bRequestType & USB_DIR_IN))
			return -1;
		switch (value >> 8) {
		case USB_DT_DEVICE:
			memcpy(reply, &dev_desc, sizeof(dev_desc));
			return sizeof(dev_desc);
		case USB_DT_DEVICE_QUALIFIER:
			memcpy(reply, &qual_desc, sizeof(qual_desc));
			return sizeof(qual_desc);
		case USB_DT_CONFIG:
			return build_config(reply, USB_DT_CONFIG);
		case USB_DT_OTHER_SPEED_CONFIG:
			return build_config(reply, USB_DT_OTHER_SPEED_CONFIG);
		case USB_DT_STRING:
			idx = value & 0xff;
			if (idx == 0) {
				reply[0] = 4;
				reply[1] = USB_DT_STRING;
				put_le16(reply + 2, 0x0409);
				return 4;
			}
			if (idx >= ARRAY_SIZE(strings))
				return -1;
			s = strings[idx];
			reply[0] = 2 + 2 * strlen(s);
			reply[1] = USB_DT_STRING;
			for (i = 0; s[i]; i++)
				put_le16(reply + 2 + 2 * i, s[i]);
			return reply[0];
		}
		return -1;
	case USB_REQ_SET_CONFIGURATION:
		if (c->bRequestType & USB_DIR_IN || value > 1)
			return -1;
		if (value == 1)
			set_configuration();
		return 0;
	case USB_REQ_GET_CONFIGURATION:
		reply[0] = emu.configured;
		return 1;
	case USB_REQ_SET_INTERFACE:
		/* Only alternate setting 0 exists. */
		return c->wIndex == 0 && value == 0 ? 0 : -1;
	case USB_REQ_GET_STATUS:
		put_le16(reply, 1);	/* self powered */
		return 2;
	}
	return -1;
}

static bool head_ok(unsigned int head, struct sbuf *errs)
{
	if (head >= T6_NUM_HEADS || !emu.heads[head].exists) {
		err_add(errs, "request for nonexistent head %u", head);
		return false;
	}
	return true;
}

/*
 * Vendor IN requests.  Returns the reply length, or -1 to stall.  Caller
 * holds emu.lock.
 */
static int vendor_in(const struct usb_ctrlrequest *c, uint8_t *reply,
		     struct sbuf *errs)
{
	unsigned int head, off, n;
	struct head_state *h;

	switch (c->bRequest) {
	case REQ_SECTION:
		memcpy(reply, t6_section_hdr, sizeof(t6_section_hdr));
		return sizeof(t6_section_hdr);
	case REQ_VRAM_SIZE:
		reply[0] = T6_VRAM_MB;
		return 1;
	case REQ_VERSION:
		if (c->wIndex >= ARRAY_SIZE(t6_versions))
			return -1;
		put_le32(reply, t6_versions[c->wIndex]);
		return 4;
	case REQ_CONN_STATUS:
		if (!head_ok(c->wValue, errs))
			return -1;
		reply[0] = emu.heads[c->wValue].connected;
		return 1;
	case REQ_TIMING_COUNT:
		if (!head_ok(c->wValue, errs))
			return -1;
		put_le32(reply, emu.heads[c->wValue].ntimings);
		return 4;
	case REQ_TIMING_TABLE:
		if (!head_ok(c->wValue, errs))
			return -1;
		h = &emu.heads[c->wValue];
		off = c->wIndex;
		n = h->ntimings * 32;
		if (off % 32)
			err_add(errs, "timing table offset %u not entry aligned",
				off);
		if (c->wLength > 512)
			err_add(errs, "timing table read of %u > 512 bytes",
				c->wLength);
		if (off >= n)
			return -1;
		n = n - off < c->wLength ? n - off : c->wLength;
		memcpy(reply, (const uint8_t *)h->table + off, n);
		return n;
	case REQ_GET_EDID:
		head = c->wIndex;
		if (!head_ok(head, errs))
			return -1;
		if (c->wValue % 128)
			err_add(errs, "EDID offset %u not block aligned",
				c->wValue);
		/* Only connected heads have a monitor; one EDID block. */
		if (!emu.heads[head].connected || c->wValue != 0)
			return -1;
		memcpy(reply, t6_edid_1080p, sizeof(t6_edid_1080p));
		return sizeof(t6_edid_1080p);
	}
	err_add(errs, "unknown vendor IN request 0x%02x", c->bRequest);
	return -1;
}

static int find_timing(const struct head_state *h, const uint8_t *t)
{
	unsigned int i;

	for (i = 0; i < h->ntimings; i++)
		if (!memcmp(h->table[i], t, 32))
			return i;
	return -1;
}

/*
 * Vendor OUT requests (data already received).  Validates the request,
 * updates the model and adds decoded fields to @b.  Returns false to stall.
 * Caller holds emu.lock.
 */
static bool vendor_out(const struct usb_ctrlrequest *c, const uint8_t *data,
		       unsigned int len, struct sbuf *b, struct sbuf *errs)
{
	unsigned int head = 0, off;
	struct head_state *h;
	int idx;

	switch (c->bRequest) {
	case REQ_MONITOR_CTRL:
		if (!head_ok(c->wValue, errs))
			return false;
		h = &emu.heads[c->wValue];
		if (c->wIndex > 1)
			err_add(errs, "monitor control wIndex %u", c->wIndex);
		if (c->wIndex == 1 && !(h->timing_set && h->sw_ready))
			err_add(errs, "head %u output on before 0x12/0x31",
				c->wValue);
		h->output_on = c->wIndex == 1;
		sb_printf(b, ",\"head\":%u,\"on\":%u", c->wValue, c->wIndex);
		return true;
	case REQ_SET_TIMING:
		if (!head_ok(c->wValue, errs))
			return false;
		h = &emu.heads[c->wValue];
		if (len != 32) {
			err_add(errs, "timing record is %u bytes", len);
			return false;
		}
		idx = find_timing(h, data);
		if (idx < 0)
			err_add(errs, "timing is not in head %u's table",
				c->wValue);
		h->timing_set = true;
		h->sw_ready = false;
		h->output_on = false;
		h->width = get_le16(data + 0x08);
		h->height = get_le16(data + 0x10);
		h->frames = 0;
		h->raw_lo = h->raw_hi = 0;
		h->shown = 0;
		sb_printf(b, ",\"head\":%u,\"table_index\":%d,\"width\":%u,\"height\":%u,\"refresh\":%u",
			  c->wValue, idx, h->width, h->height, data[4]);
		return true;
	case REQ_SOFTWARE_READY:
		if (!head_ok(c->wValue, errs))
			return false;
		h = &emu.heads[c->wValue];
		if (!h->timing_set)
			err_add(errs, "0x31 before 0x12 on head %u", c->wValue);
		h->sw_ready = true;
		sb_printf(b, ",\"head\":%u", c->wValue);
		return true;
	case REQ_CURSOR2_POS:
		head = 1;
		/* fall through */
	case REQ_CURSOR1_POS:
		if (!head_ok(head, errs))
			return false;
		/* Negative coordinates are one's complement of the magnitude. */
		sb_printf(b, ",\"head\":%u,\"x\":%d,\"y\":%d", head,
			  c->wValue & 0x8000 ? -(int)(uint16_t)~c->wValue : c->wValue,
			  c->wIndex & 0x8000 ? -(int)(uint16_t)~c->wIndex : c->wIndex);
		return true;
	case REQ_CURSOR2_STATE:
		head = 1;
		/* fall through */
	case REQ_CURSOR1_STATE:
		if (!head_ok(head, errs))
			return false;
		if (c->wIndex > 1)
			err_add(errs, "cursor state wIndex %u", c->wIndex);
		if (c->wValue > CURSOR_MAX_SLOT)
			err_add(errs, "cursor state slot %u", c->wValue);
		sb_printf(b, ",\"head\":%u,\"slot\":%u,\"visible\":%u", head,
			  c->wValue, c->wIndex);
		return true;
	case REQ_CURSOR2_SHAPE:
		head = 1;
		/* fall through */
	case REQ_CURSOR1_SHAPE:
		if (!head_ok(head, errs))
			return false;
		h = &emu.heads[head];
		off = c->wIndex;
		if (len > 4096)
			err_add(errs, "cursor chunk of %u bytes", len);
		if (c->wValue > CURSOR_MAX_SLOT)
			err_add(errs, "cursor shape slot %u", c->wValue);
		if (off + len > sizeof(h->cursor)) {
			err_add(errs, "cursor data beyond 128x128");
			return false;
		}
		memcpy(h->cursor + off, data, len);
		sb_printf(b, ",\"head\":%u,\"offset\":%u", head, off);
		if (off == 0 && len >= CURSOR_HDR_LEN) {
			unsigned int type = get_le16(data), w = get_le16(data + 2);
			unsigned int hh = get_le16(data + 4);
			unsigned int pitch = get_le16(data + 6);

			sb_printf(b, ",\"type\":%u,\"width\":%u,\"height\":%u,\"pitch\":%u",
				  type, w, hh, pitch);
			if (type != 1 || pitch != w * 4 || !w || !hh ||
			    w > 128 || hh > 128)
				err_add(errs, "bad cursor header %u %ux%u pitch %u",
					type, w, hh, pitch);
		}
		return true;
	case REQ_CANCEL_BULK:
		return true;
	case 0x13: case 0x32:
		err_add(errs, "forbidden ROM write request 0x%02x", c->bRequest);
		return false;
	case 0x30:
		err_add(errs, "forbidden hardware reset request");
		return false;
	}
	err_add(errs, "unknown vendor OUT request 0x%02x", c->bRequest);
	return false;
}

static const char *req_name(uint8_t type, uint8_t req)
{
	if ((type & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		switch (req) {
		case USB_REQ_GET_DESCRIPTOR: return "GET_DESCRIPTOR";
		case USB_REQ_SET_CONFIGURATION: return "SET_CONFIGURATION";
		case USB_REQ_SET_INTERFACE: return "SET_INTERFACE";
		case USB_REQ_GET_STATUS: return "GET_STATUS";
		}
		return "standard";
	}
	switch (req) {
	case REQ_MONITOR_CTRL: return "MONITOR_CTRL";
	case REQ_CURSOR1_POS: case REQ_CURSOR2_POS: return "CURSOR_POS";
	case REQ_CURSOR1_STATE: case REQ_CURSOR2_STATE: return "CURSOR_STATE";
	case REQ_CURSOR1_SHAPE: case REQ_CURSOR2_SHAPE: return "CURSOR_SHAPE";
	case REQ_SET_TIMING: return "SET_TIMING";
	case REQ_CANCEL_BULK: return "CANCEL_BULK_OUT";
	case REQ_SOFTWARE_READY: return "SOFTWARE_READY";
	case REQ_GET_EDID: return "GET_EDID";
	case REQ_TIMING_COUNT: return "TIMING_COUNT";
	case REQ_CONN_STATUS: return "CONN_STATUS";
	case REQ_VRAM_SIZE: return "VRAM_SIZE";
	case REQ_TIMING_TABLE: return "TIMING_TABLE";
	case REQ_VERSION: return "VERSION";
	case REQ_SECTION: return "SECTION";
	}
	return "unknown";
}

static void handle_control(const struct usb_ctrlrequest *c)
{
	static uint8_t buf[CURSOR_MAX_BYTES];
	struct sbuf b = { 0 }, errs = { 0 };
	bool in = c->bRequestType & USB_DIR_IN, ok, dummy;
	unsigned int type = c->bRequestType & USB_TYPE_MASK;
	int len = 0, ret;

	sb_printf(&b, "\"ev\":\"ctrl\",\"name\":\"%s\",\"bmRequestType\":%u,\"bRequest\":%u,\"wValue\":%u,\"wIndex\":%u,\"wLength\":%u",
		  req_name(c->bRequestType, c->bRequest), c->bRequestType,
		  c->bRequest, c->wValue, c->wIndex, c->wLength);

	if (type == USB_TYPE_STANDARD) {
		len = std_request(c, buf, &dummy);
		ok = len >= 0;
	} else if (type == USB_TYPE_VENDOR && in) {
		pthread_mutex_lock(&emu.lock);
		len = vendor_in(c, buf, &errs);
		pthread_mutex_unlock(&emu.lock);
		ok = len >= 0;
	} else if (type == USB_TYPE_VENDOR) {
		/* Receive the data stage first, then decide. */
		len = 0;
		if (c->wLength) {
			len = ep0_read(buf, c->wLength);
			if (len < 0) {
				fprintf(stderr, "t6emu: EP0_READ: %s\n",
					strerror(errno));
				goto out;
			}
			if (len != c->wLength)
				err_add(&errs, "short OUT data stage %d/%u",
					len, c->wLength);
		}
		pthread_mutex_lock(&emu.lock);
		ok = vendor_out(c, buf, len, &b, &errs);
		pthread_mutex_unlock(&emu.lock);
		/* A data stage already completed the transfer. */
		if (c->wLength) {
			sb_printf(&b, ",\"data\":\"");
			sb_hex(&b, buf, len);
			sb_printf(&b, "\",\"stall\":false");
			goto out;
		}
	} else {
		ok = false;
		err_add(&errs, "class request type 0x%02x", c->bRequestType);
	}

	if (!ok) {
		ep0_stall();
		sb_printf(&b, ",\"stall\":true");
		goto out;
	}
	if (in) {
		if (len > c->wLength)
			len = c->wLength;
		ret = ep0_write(buf, len);
		if (ret < 0)
			fprintf(stderr, "t6emu: EP0_WRITE: %s\n", strerror(errno));
		sb_printf(&b, ",\"data\":\"");
		sb_hex(&b, buf, len);
		sb_printf(&b, "\"");
	} else if (!c->wLength) {
		/* Zero-length OUT: acknowledge with an empty status stage. */
		ret = ep0_read(buf, 0);
		if (ret < 0)
			fprintf(stderr, "t6emu: EP0 ack: %s\n", strerror(errno));
	}
	sb_printf(&b, ",\"stall\":false");
out:
	pthread_mutex_lock(&emu.lock);
	log_event(&b, &errs);
	pthread_mutex_unlock(&emu.lock);
	free(b.s);
	free(errs.s);
}

/* --- Bulk OUT ---------------------------------------------------------- */

#define BULK_CHUNK	(64 * 1024)

struct bulk_io {
	struct usb_raw_ep_io io;
	uint8_t data[BULK_CHUNK];
};

/* Read exactly @len bytes from bulk OUT, over as many requests as needed. */
static int bulk_read_exact(uint8_t *dst, size_t len)
{
	static struct bulk_io r;
	size_t got = 0;
	int ret;

	while (got < len) {
		size_t want = len - got < BULK_CHUNK ? len - got : BULK_CHUNK;

		r.io.ep = emu.h_bulk_out;
		r.io.flags = 0;
		r.io.length = want;
		ret = ioctl(emu.fd, USB_RAW_IOCTL_EP_READ, &r);
		if (ret < 0) {
			if (stopping)
				return -1;
			if (errno == EINTR)
				continue;
			fprintf(stderr, "t6emu: bulk EP_READ: %s\n",
				strerror(errno));
			return -1;
		}
		memcpy(dst + got, r.data, ret);
		got += ret;
		/* A short read means the host ended the transfer early. */
		if ((size_t)ret < want && got < len)
			return got;
	}
	return got;
}

/* Find SOF0 in a JPEG and return width/height/luma sampling. */
static bool jpeg_sof(const uint8_t *j, size_t n, unsigned int *w,
		     unsigned int *h, unsigned int *ncomp, uint8_t *ysamp)
{
	size_t i = 2;

	while (i + 4 <= n && j[i] == 0xff) {
		uint8_t m = j[i + 1];
		unsigned int seglen = j[i + 2] << 8 | j[i + 3];

		if (m == 0xc0 && i + 4 + 8 <= n) {
			*h = j[i + 5] << 8 | j[i + 6];
			*w = j[i + 7] << 8 | j[i + 8];
			*ncomp = j[i + 9];
			*ysamp = j[i + 11];
			return true;
		}
		if (m == 0xda)
			break;
		i += 2 + seglen;
	}
	return false;
}

/* Queue a frame-done event for the interrupt thread.  Caller holds lock. */
static void queue_frame_done(uint32_t fence)
{
	unsigned int next = (emu.int_tail + 1) % ARRAY_SIZE(emu.int_queue);

	if (next == emu.int_head) {
		fprintf(stderr, "t6emu: interrupt queue full, dropping fence %u\n",
			fence);
		return;
	}
	emu.int_queue[emu.int_tail] = fence;
	emu.int_tail = next;
	pthread_cond_signal(&emu.int_cond);
}

static void check_fence(struct sbuf *errs, uint32_t fence)
{
	if (emu.any_fence && (int32_t)(fence - emu.last_fence) <= 0)
		err_add(errs, "fence %u does not increase (last %u)", fence,
			emu.last_fence);
	emu.any_fence = true;
	emu.last_fence = fence;
}

/*
 * A raw RGB24 frame: the video header sits immediately before the pixels,
 * which are packed B,G,R lines that the chip scans out as they are.  Saved
 * as a binary PPM (R,G,B) so the frame can be compared exactly.
 */
static void handle_raw(struct head_state *h, struct sbuf *b,
		       struct sbuf *errs, uint32_t addr, uint32_t len,
		       const uint8_t *p, uint32_t psize, uint32_t tfmt,
		       unsigned int ypitch, unsigned int uvpitch,
		       uint32_t yaddr, uint32_t uvaddr, uint32_t vaddr,
		       unsigned int flags)
{
	uint32_t want = h->width * 3 * h->height;
	uint64_t end = (uint64_t)addr + len;
	char name[64], path[4096];
	unsigned int i, x, y;
	FILE *f;

	sb_printf(b, ",\"raw\":1");
	if (tfmt != 9)
		err_add(errs, "raw frame target format %u, expected 9 (RGB24)",
			tfmt);
	if (ypitch != h->width * 3 || uvpitch)
		err_add(errs, "raw pitches %u/%u, expected %u/0", ypitch,
			uvpitch, h->width * 3);
	if (uvaddr != yaddr || vaddr)
		err_add(errs, "raw plane addresses %u/%u, expected %u/0", uvaddr,
			vaddr, yaddr);
	if (flags)
		err_add(errs, "raw frame flags 0x%02x", flags);
	if (addr + VIDEO_HDR_LEN != yaddr)
		err_add(errs, "raw header at %u does not end at the pixels (%u)",
			addr, yaddr);
	if (yaddr % 32)
		err_add(errs, "raw surface %u not 32-byte aligned", yaddr);
	if (addr < h->ring_base || end > h->ring_end)
		err_add(errs, "raw object [%u, +%u) outside head region [%u, %u)",
			addr, len, h->ring_base, h->ring_end);
	for (i = 0; i < FB_SLOTS; i++)
		if (addr < h->fb[i] + FB_SLOT_SIZE && end > h->fb[i])
			err_add(errs, "raw object [%u, +%u) overlaps NV12 slot %u",
				addr, len, i);
	if (h->shown && yaddr == h->shown)
		err_add(errs, "raw frame overwrites the surface on screen (%u)",
			yaddr);
	for (i = 36; i < 47; i++)
		if (p[i]) {
			err_add(errs, "reserved video header byte 0x%02x is %u",
				i, p[i]);
			break;
		}
	if (psize != len - VIDEO_HDR_LEN || psize != want) {
		err_add(errs, "raw payload_size %u, object carries %u, mode needs %u",
			psize, len - VIDEO_HDR_LEN, want);
		return;
	}

	if (!h->raw_hi || addr < h->raw_lo)
		h->raw_lo = addr;
	if (end > h->raw_hi)
		h->raw_hi = end;
	h->shown = yaddr;

	emu.frames++;
	emu.raw_frames++;
	snprintf(name, sizeof(name), "frame-%05lu.ppm", emu.frames);
	snprintf(path, sizeof(path), "%s/%s", emu.outdir, name);
	f = fopen(path, "wb");
	if (f) {
		const uint8_t *px = p + VIDEO_HDR_LEN;
		bool ok = fprintf(f, "P6\n%u %u\n255\n", h->width,
				  h->height) > 0;

		for (y = 0; ok && y < h->height; y++)
			for (x = 0; ok && x < h->width; x++, px += 3) {
				uint8_t rgb[3] = { px[2], px[1], px[0] };

				ok = fwrite(rgb, 1, 3, f) == 3;
			}
		if (fclose(f) || !ok)
			err_add(errs, "cannot write frame file");
	} else {
		err_add(errs, "cannot write frame file");
	}
	sb_printf(b, ",\"frame\":%lu,\"file\":\"%s\"", emu.frames, name);
}

/* Validate one complete display object and save its JPEG. */
static void handle_video(const uint8_t *hdr, const uint8_t *p, uint32_t len)
{
	struct sbuf b = { 0 }, errs = { 0 };
	uint32_t sig = get_le32(hdr), total = get_le32(hdr + 4);
	uint32_t addr = get_le32(hdr + 8), frag_len = get_le32(hdr + 12);
	uint32_t frag_off = get_le32(hdr + 16);
	uint8_t more = hdr[20];
	uint32_t cmd = 0, psize = 0, fence = 0, tfmt = 0, sfmt = 0;
	uint32_t yaddr = 0, uvaddr = 0, vaddr = 0, jlen = 0;
	unsigned int ypitch = 0, uvpitch = 0, flags = 0, i, slot = 0;
	unsigned int head = 0, jw = 0, jh = 0, ncomp = 0;
	struct head_state *h = NULL;
	char name[64] = "";
	uint8_t ysamp = 0;
	bool reset_ok;

	pthread_mutex_lock(&emu.lock);
	emu.objects++;
	emu.bytes += BULK_HDR_LEN + len;

	sb_printf(&b, "\"ev\":\"bulk\",\"signature\":%u,\"length\":%u,\"addr\":%u,\"frag_length\":%u,\"frag_offset\":%u,\"more\":%u",
		  sig, total, addr, frag_len, frag_off, more);
	if (sig != 0)
		err_add(&errs, "signature %u (only display objects allowed)", sig);
	if (frag_len != total || frag_off || more)
		err_add(&errs, "fragmented object (frag %u@%u more %u)",
			frag_len, frag_off, more);
	for (i = 21; i < BULK_HDR_LEN; i++)
		if (hdr[i]) {
			err_add(&errs, "reserved header byte 0x%02x is %u", i,
				hdr[i]);
			break;
		}
	if (len < VIDEO_HDR_LEN) {
		err_add(&errs, "object of %u bytes has no video header", len);
		goto out;
	}

	cmd = get_le32(p);
	psize = get_le32(p + 4);
	fence = get_le32(p + 8);
	tfmt = get_le32(p + 12);
	ypitch = get_le16(p + 16);
	uvpitch = get_le16(p + 18);
	yaddr = get_le32(p + 20);
	uvaddr = get_le32(p + 24);
	vaddr = get_le32(p + 28);
	sfmt = get_le32(p + 32);
	flags = p[47];
	sb_printf(&b, ",\"command\":%u,\"payload_size\":%u,\"fence\":%u,\"target_format\":%u,\"y_pitch\":%u,\"uv_pitch\":%u,\"y_addr\":%u,\"uv_addr\":%u,\"v_addr\":%u,\"source_format\":%u,\"flags\":%u",
		  cmd, psize, fence, tfmt, ypitch, uvpitch, yaddr, uvaddr,
		  vaddr, sfmt, flags);

	if (cmd != 3 && cmd != 4) {
		err_add(&errs, "video command %u is not a flip", cmd);
		goto out;
	}
	head = cmd - 3;
	h = &emu.heads[head];
	sb_printf(&b, ",\"head\":%u", head);
	if (!h->exists || !h->timing_set) {
		err_add(&errs, "flip on head %u without a mode set", head);
		goto out;
	}
	if (!h->output_on)
		err_add(&errs, "flip on head %u while its output is off", head);
	if (sfmt == 9) {
		check_fence(&errs, fence);
		handle_raw(h, &b, &errs, addr, len, p, psize, tfmt, ypitch,
			   uvpitch, yaddr, uvaddr, vaddr, flags);
		goto done;
	}
	if (h->raw_hi && addr < h->raw_hi &&
	    addr + (uint64_t)len > h->raw_lo)
		err_add(&errs, "JPEG object [%u, +%u) overlaps raw frames [%u, %u)",
			addr, len, h->raw_lo, h->raw_hi);
	if (addr < h->ring_base || addr + (uint64_t)len > h->ring_end)
		err_add(&errs, "object [%u, +%u) outside head %u ring [%u, %u)",
			addr, len, head, h->ring_base, h->ring_end);
	if (addr % MiB)
		err_add(&errs, "ring address %u not MiB aligned", addr);
	if (tfmt != 6)
		err_add(&errs, "target format %u, expected 6 (NV12)", tfmt);
	if (sfmt != 13)
		err_add(&errs, "source format %u, expected 13 (JPEG)", sfmt);
	if (ypitch != ALIGN(h->width, 32) || uvpitch != ALIGN(h->width, 32))
		err_add(&errs, "pitches %u/%u, expected %u", ypitch, uvpitch,
			ALIGN(h->width, 32));
	for (slot = 0; slot < FB_SLOTS && h->fb[slot] != yaddr; slot++)
		;
	if (slot == FB_SLOTS)
		err_add(&errs, "Y address %u is not a framebuffer slot", yaddr);
	else
		h->shown = yaddr;
	if (uvaddr != yaddr + ALIGN(h->width, 32) * ALIGN(h->height, 32) +
		      JPEG_PAD)
		err_add(&errs, "UV address %u inconsistent with Y %u", uvaddr,
			yaddr);
	if (vaddr)
		err_add(&errs, "V address %u, expected 0", vaddr);
	for (i = 36; i < 47; i++)
		if (p[i]) {
			err_add(&errs, "reserved video header byte 0x%02x is %u",
				i, p[i]);
			break;
		}
	if (flags & ~0x80)
		err_add(&errs, "unknown video flags 0x%02x", flags);

	/* Reset is required for the first frames, else only on ring wrap. */
	reset_ok = h->frames >= RESET_FRAMES || (flags & 0x80);
	if (!reset_ok)
		err_add(&errs, "frame %u after mode set lacks the reset flag",
			h->frames);
	if (h->frames >= RESET_FRAMES && (flags & 0x80) &&
	    addr != h->ring_base)
		err_add(&errs, "reset flag without a ring wrap");
	h->frames++;

	check_fence(&errs, fence);

	if (psize != len - VIDEO_HDR_LEN) {
		err_add(&errs, "payload_size %u, object carries %u", psize,
			len - VIDEO_HDR_LEN);
		goto done;
	}
	if (psize < JPEG_PAD + 4) {
		err_add(&errs, "payload too small for a JPEG");
		goto done;
	}
	jlen = psize - JPEG_PAD;
	sb_printf(&b, ",\"jpeg_length\":%u", jlen);
	for (i = 0; i < JPEG_PAD; i++)
		if (p[VIDEO_HDR_LEN + jlen + i]) {
			err_add(&errs, "JPEG padding byte %u is not zero", i);
			break;
		}
	if (p[VIDEO_HDR_LEN] != 0xff || p[VIDEO_HDR_LEN + 1] != 0xd8)
		err_add(&errs, "JPEG does not start with FFD8");
	if (p[VIDEO_HDR_LEN + jlen - 2] != 0xff ||
	    p[VIDEO_HDR_LEN + jlen - 1] != 0xd9)
		err_add(&errs, "JPEG does not end with FFD9");
	if (!jpeg_sof(p + VIDEO_HDR_LEN, jlen, &jw, &jh, &ncomp, &ysamp))
		err_add(&errs, "JPEG has no baseline SOF0");
	else if (jw != h->width || jh != h->height || ncomp != 3 ||
		 ysamp != 0x22)
		err_add(&errs, "JPEG is %ux%u, %u components, Y sampling 0x%02x; mode is %ux%u 4:2:0",
			jw, jh, ncomp, ysamp, h->width, h->height);

	emu.frames++;
	snprintf(name, sizeof(name), "frame-%05lu.jpg", emu.frames);
	{
		char path[4096];
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s", emu.outdir, name);
		f = fopen(path, "wb");
		if (!f || fwrite(p + VIDEO_HDR_LEN, 1, jlen, f) != jlen)
			err_add(&errs, "cannot write frame file");
		if (f)
			fclose(f);
	}
	sb_printf(&b, ",\"frame\":%lu,\"file\":\"%s\",\"slot\":%u", emu.frames,
		  name, slot);

done:
	if (!emu.no_fence)
		queue_frame_done(fence);
out:
	log_event(&b, &errs);
	write_status();
	pthread_mutex_unlock(&emu.lock);
	free(b.s);
	free(errs.s);
}

static void *bulk_thread_fn(void *arg)
{
	uint8_t hdr[BULK_HDR_LEN];
	uint8_t *payload = malloc(MAX_OBJECT);
	int ret;

	(void)arg;
	if (!payload)
		die("out of memory");
	while (!stopping) {
		uint32_t len;

		ret = bulk_read_exact(hdr, BULK_HDR_LEN);
		if (ret < 0)
			break;
		if (ret != BULK_HDR_LEN) {
			log_simple("bulk_short", "\"bytes\":%d", ret);
			continue;
		}
		len = get_le32(hdr + 4);
		if (len == 0 || len > MAX_OBJECT) {
			struct sbuf b = { 0 }, errs = { 0 };

			sb_printf(&b, "\"ev\":\"bulk\",\"header\":\"");
			sb_hex(&b, hdr, BULK_HDR_LEN);
			sb_printf(&b, "\"");
			err_add(&errs, "implausible object length %u", len);
			pthread_mutex_lock(&emu.lock);
			log_event(&b, &errs);
			pthread_mutex_unlock(&emu.lock);
			free(b.s);
			free(errs.s);
			continue;
		}
		ret = bulk_read_exact(payload, len);
		if (ret < 0)
			break;
		if ((uint32_t)ret != len) {
			log_simple("bulk_short", "\"bytes\":%d,\"expected\":%u",
				   ret, len);
			continue;
		}
		handle_video(hdr, payload, len);
	}
	free(payload);
	return NULL;
}

/* --- Interrupt IN ------------------------------------------------------ */

static void *int_thread_fn(void *arg)
{
	struct {
		struct usb_raw_ep_io io;
		uint8_t data[INT_MAXP];
	} r;
	uint32_t fence;
	int ret;

	(void)arg;
	for (;;) {
		pthread_mutex_lock(&emu.lock);
		while (!stopping && emu.int_head == emu.int_tail)
			pthread_cond_wait(&emu.int_cond, &emu.lock);
		if (stopping) {
			pthread_mutex_unlock(&emu.lock);
			break;
		}
		fence = emu.int_queue[emu.int_head];
		emu.int_head = (emu.int_head + 1) % ARRAY_SIZE(emu.int_queue);
		pthread_mutex_unlock(&emu.lock);

		/* Display function, fence at 0x0C, "frame done" at 0x13. */
		memset(&r, 0, sizeof(r));
		r.io.ep = emu.h_int_in;
		r.io.length = INT_MAXP;
		r.data[0x00] = 0x04;
		put_le32(r.data + 0x0c, fence);
		r.data[0x13] = 0x04;
		do {
			ret = ioctl(emu.fd, USB_RAW_IOCTL_EP_WRITE, &r);
		} while (ret < 0 && errno == EINTR && !stopping);
		if (ret < 0) {
			if (!stopping)
				fprintf(stderr, "t6emu: interrupt EP_WRITE: %s\n",
					strerror(errno));
			break;
		}
		log_simple("int", "\"type\":\"frame_done\",\"fence\":%u", fence);
	}
	return NULL;
}

/* --- Main loop --------------------------------------------------------- */

/*
 * Stop a worker that may be blocked in a raw-gadget ioctl.  The signal can
 * arrive just before the thread enters the ioctl, so keep kicking it.
 */
static void stop_thread(pthread_t t)
{
	struct timespec ts;

	for (;;) {
		pthread_mutex_lock(&emu.lock);
		pthread_cond_broadcast(&emu.int_cond);
		pthread_mutex_unlock(&emu.lock);
		pthread_kill(t, SIGUSR1);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		if (pthread_timedjoin_np(t, NULL, &ts) == 0)
			return;
	}
}

static void on_signal(int sig)
{
	if (sig != SIGUSR1)
		stopping = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: t6emu --outdir DIR [--head0 connected|disconnected]\n"
		"             [--head1 connected|disconnected] [--no-fence]\n"
		"             [--udc-driver dummy_udc] [--udc-device dummy_udc.0]\n");
	exit(2);
}

static bool parse_conn(const char *s)
{
	if (!strcmp(s, "connected"))
		return true;
	if (!strcmp(s, "disconnected"))
		return false;
	usage();
	return false;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "outdir", required_argument, NULL, 'o' },
		{ "head0", required_argument, NULL, '0' },
		{ "head1", required_argument, NULL, '1' },
		{ "no-fence", no_argument, NULL, 'n' },
		{ "udc-driver", required_argument, NULL, 'd' },
		{ "udc-device", required_argument, NULL, 'D' },
		{ }
	};
	const char *udc_driver = "dummy_udc", *udc_device = "dummy_udc.0";
	bool conn0 = true, conn1 = false;
	struct usb_raw_init init = { 0 };
	struct sigaction sa = { 0 };
	char path[4096];
	int c;

	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'o': emu.outdir = optarg; break;
		case '0': conn0 = parse_conn(optarg); break;
		case '1': conn1 = parse_conn(optarg); break;
		case 'n': emu.no_fence = true; break;
		case 'd': udc_driver = optarg; break;
		case 'D': udc_device = optarg; break;
		default: usage();
		}
	}
	if (!emu.outdir)
		usage();

	clock_gettime(CLOCK_MONOTONIC, &emu.t0);
	snprintf(path, sizeof(path), "%s/log.jsonl", emu.outdir);
	emu.log = fopen(path, "w");
	if (!emu.log)
		die("cannot create %s: %s", path, strerror(errno));
	model_init(conn0, conn1);
	pthread_mutex_lock(&emu.lock);
	write_status();
	pthread_mutex_unlock(&emu.lock);

	/* No SA_RESTART: signals must interrupt the blocking ioctls. */
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	emu.fd = open("/dev/raw-gadget", O_RDWR);
	if (emu.fd < 0)
		die("open /dev/raw-gadget: %s", strerror(errno));
	snprintf((char *)init.driver_name, UDC_NAME_LENGTH_MAX, "%s", udc_driver);
	snprintf((char *)init.device_name, UDC_NAME_LENGTH_MAX, "%s", udc_device);
	init.speed = USB_SPEED_HIGH;
	if (ioctl(emu.fd, USB_RAW_IOCTL_INIT, &init) < 0)
		die("USB_RAW_IOCTL_INIT: %s", strerror(errno));
	if (ioctl(emu.fd, USB_RAW_IOCTL_RUN, 0) < 0)
		die("USB_RAW_IOCTL_RUN: %s", strerror(errno));
	log_simple("start", "\"head0_connected\":%d,\"head1_connected\":%d,\"fences\":%d",
		   conn0, conn1, !emu.no_fence);

	while (!stopping) {
		struct {
			struct usb_raw_event ev;
			uint8_t data[sizeof(struct usb_ctrlrequest)];
		} e;

		e.ev.type = 0;
		e.ev.length = sizeof(e.data);
		if (ioctl(emu.fd, USB_RAW_IOCTL_EVENT_FETCH, &e) < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "t6emu: EVENT_FETCH: %s\n", strerror(errno));
			break;
		}
		switch (e.ev.type) {
		case USB_RAW_EVENT_CONNECT:
			choose_endpoints();
			log_simple("connect", "\"bulk_in\":%u,\"bulk_out\":%u,\"int_in\":%u",
				   emu.ep_bulk_in, emu.ep_bulk_out, emu.ep_int_in);
			break;
		case USB_RAW_EVENT_CONTROL:
			handle_control((struct usb_ctrlrequest *)e.data);
			break;
		case USB_RAW_EVENT_RESET:
			log_simple("reset", NULL);
			break;
		case USB_RAW_EVENT_DISCONNECT:
			log_simple("disconnect", NULL);
			break;
		case USB_RAW_EVENT_SUSPEND:
			log_simple("suspend", NULL);
			break;
		case USB_RAW_EVENT_RESUME:
			log_simple("resume", NULL);
			break;
		default:
			log_simple("unknown_event", "\"type\":%u", e.ev.type);
		}
	}

	/* Wake the workers out of their blocking calls and wait for them. */
	stopping = 1;
	if (emu.configured) {
		stop_thread(emu.bulk_thread);
		stop_thread(emu.int_thread);
	}
	pthread_mutex_lock(&emu.lock);
	write_status();
	pthread_mutex_unlock(&emu.lock);
	log_simple("exit", "\"frames\":%lu,\"objects\":%lu,\"bytes\":%lu,\"errors\":%lu",
		   emu.frames, emu.objects, emu.bytes, emu.errors);
	fclose(emu.log);
	close(emu.fd);
	return 0;
}
