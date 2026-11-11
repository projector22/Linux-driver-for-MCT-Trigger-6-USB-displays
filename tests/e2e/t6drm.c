// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Gareth Palmer
 *
 * t6drm: a minimal KMS client that drives mct_t6 through a known sequence
 * for the end-to-end test, using raw DRM ioctls (no libdrm).
 *
 *  1. find the DRM device whose driver is "mct_t6" and its connected
 *     connector, and pick 1920x1080@60;
 *  2. draw a deterministic test pattern into a dumb buffer, ADDFB2 it
 *     (XRGB8888) and SETCRTC;
 *  3. change sub-rectangles (one touching the bottom-right corner) and
 *     report each with DIRTYFB;
 *  4. exercise the hardware cursor through DRM_IOCTL_MODE_CURSOR2: set a
 *     64x64 ARGB shape, move it (also partly off-screen), hide it;
 *  5. disable the CRTC.
 *
 * After every screen change it waits for the emulator (t6emu) to receive
 * the resulting frames, by polling the frame counter in t6emu's status
 * file, and saves the image it expects the display to show as raw RGB24
 * (expected-NN.rgb).  manifest.json records, for each step, the changed
 * rectangle and the range of emulator frames it produced, which is what
 * verify.py checks the decoded JPEGs against.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define WIDTH		1920
#define HEIGHT		1080
#define REFRESH		60
#define CURSOR_SIZE	64
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

static const char *outdir, *status_path;
static int drm_fd = -1;
static FILE *manifest;
static unsigned int settle_ms = 1500, timeout_ms = 120000;
static unsigned int failures;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "t6drm: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void xioctl(unsigned long req, void *arg, const char *what)
{
	int ret;

	do {
		ret = ioctl(drm_fd, req, arg);
	} while (ret < 0 && (errno == EINTR || errno == EAGAIN));
	if (ret < 0)
		die("%s: %s", what, strerror(errno));
}

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

/* --- Talking to the emulator ------------------------------------------- */

/* Frames t6emu has received so far, from its status file. */
static long emu_frames(void)
{
	char line[128];
	long frames = -1;
	FILE *f = fopen(status_path, "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "frames %ld", &frames) == 1)
			break;
	fclose(f);
	return frames;
}

/*
 * Wait until at least one frame newer than @before has arrived and no
 * further frame has arrived for settle_ms.  Returns the last frame number.
 */
static long wait_frames(long before)
{
	uint64_t start = now_ms(), last_change = 0;
	long cur = before, n;

	for (;;) {
		n = emu_frames();
		if (n != cur) {
			cur = n;
			last_change = now_ms();
		}
		if (cur > before && now_ms() - last_change >= settle_ms)
			return cur;
		if (now_ms() - start > timeout_ms)
			die("timed out waiting for frames after %ld (now %ld)",
			    before, cur);
		sleep_ms(20);
	}
}

/* Wait until a frame newer than @before has arrived; no settling. */
static void wait_first_frame(long before)
{
	uint64_t start = now_ms();

	while (emu_frames() <= before) {
		if (now_ms() - start > timeout_ms)
			die("timed out waiting for a frame after %ld", before);
		sleep_ms(5);
	}
}

/* --- Pattern ------------------------------------------------------------- */

/*
 * Deterministic test pattern, a function of position and a "variant" so
 * every update visibly changes the pixels it covers: colour bars across
 * the top, smooth gradients, a checkerboard, and fine diagonal lines.  All
 * of it is easy to compare after lossy JPEG compression.
 */
static uint32_t pattern(unsigned int x, unsigned int y, unsigned int variant)
{
	static const uint32_t bars[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000,
	};
	unsigned int r, g, b;

	if (variant == 0 && y < 120)
		return bars[x * 8 / WIDTH];
	if (variant == 0 && y >= 900 && x < 960)
		return (x / 40 + y / 40) % 2 ? 0xe0e0e0 : 0x202020;

	r = (x * 255 / (WIDTH - 1) + variant * 85) & 0xff;
	g = (y * 255 / (HEIGHT - 1) + variant * 45) & 0xff;
	b = ((x + y) / 8 * 16 + variant * 64) & 0xff;
	if (variant && ((x + y) / 24) % 2)
		r = 255 - r;
	if (variant == 0 && ((x + 2 * y) % 64) < 3)
		r = g = b = 0;
	return r << 16 | g << 8 | b;
}

/* --- Buffers --------------------------------------------------------------- */

struct dumb {
	uint32_t handle, pitch, fb_id;
	uint64_t size;
	uint32_t *map;
};

static void dumb_create(struct dumb *d, unsigned int w, unsigned int h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_map_dumb m = { 0 };

	xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &c, "CREATE_DUMB");
	d->handle = c.handle;
	d->pitch = c.pitch;
	d->size = c.size;
	m.handle = d->handle;
	xioctl(DRM_IOCTL_MODE_MAP_DUMB, &m, "MAP_DUMB");
	d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      drm_fd, m.offset);
	if (d->map == MAP_FAILED)
		die("mmap dumb buffer: %s", strerror(errno));
}

static void dumb_destroy(struct dumb *d)
{
	struct drm_mode_destroy_dumb x = { .handle = d->handle };

	munmap(d->map, d->size);
	xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &x, "DESTROY_DUMB");
}

static void fill_rect(struct dumb *d, unsigned int x0, unsigned int y0,
		      unsigned int w, unsigned int h, unsigned int variant)
{
	unsigned int x, y;

	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			d->map[y * (d->pitch / 4) + x] = pattern(x, y, variant);
}

/* Save what the display should show now, as raw RGB24. */
static void save_expected(const struct dumb *d, const char *name)
{
	static uint8_t row[WIDTH * 3];
	char path[4096];
	unsigned int x, y;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", outdir, name);
	f = fopen(path, "wb");
	if (!f)
		die("create %s: %s", path, strerror(errno));
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			uint32_t p = d->map[y * (d->pitch / 4) + x];

			row[3 * x] = p >> 16;
			row[3 * x + 1] = p >> 8;
			row[3 * x + 2] = p;
		}
		if (fwrite(row, 3, WIDTH, f) != WIDTH)
			die("write %s", path);
	}
	if (fclose(f))
		die("write %s: %s", path, strerror(errno));
}

/* --- Device discovery ---------------------------------------------------- */

static int open_card(const char *want)
{
	char path[64], name[64];
	unsigned int i;

	for (i = 0; i < 16; i++) {
		struct drm_version v = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%u", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof(name));
		v.name = name;
		v.name_len = sizeof(name) - 1;
		if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, want)) {
			printf("t6drm: %s is %s %d.%d.%d\n", path, name,
			       v.version_major, v.version_minor,
			       v.version_patchlevel);
			return fd;
		}
		close(fd);
	}
	return -1;
}

struct output {
	uint32_t connector_id, crtc_id;
	struct drm_mode_modeinfo mode;
};

static const char *const conn_status[] = { "?", "connected", "disconnected",
					   "unknown" };

/* Find a connected connector with a 1920x1080@60 mode, and a CRTC for it. */
static void find_output(struct output *out)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t *crtcs, *conns;
	unsigned int i, j;
	bool found = false;

	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	crtcs = calloc(res.count_crtcs, sizeof(*crtcs));
	conns = calloc(res.count_connectors, sizeof(*conns));
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.count_fbs = res.count_encoders = 0;
	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	printf("t6drm: %u CRTC(s), %u connector(s)\n", res.count_crtcs,
	       res.count_connectors);

	for (i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		struct drm_mode_modeinfo *modes;
		uint32_t *encs;
		struct drm_mode_get_encoder e = { 0 };

		/* First call probes and returns counts, second fetches. */
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR");
		modes = calloc(c.count_modes + 1, sizeof(*modes));
		encs = calloc(c.count_encoders + 1, sizeof(*encs));
		c.modes_ptr = (uintptr_t)modes;
		c.encoders_ptr = (uintptr_t)encs;
		c.count_props = 0;
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR");
		printf("t6drm: connector %u type %u: %s, %u modes\n",
		       c.connector_id, c.connector_type,
		       conn_status[c.connection < 4 ? c.connection : 0],
		       c.count_modes);

		if (c.connection != 1 || found || !c.count_encoders)
			goto next;
		for (j = 0; j < c.count_modes; j++)
			if (modes[j].hdisplay == WIDTH &&
			    modes[j].vdisplay == HEIGHT &&
			    modes[j].vrefresh == REFRESH)
				break;
		if (j == c.count_modes) {
			printf("t6drm: no %ux%u@%u mode on connector %u\n",
			       WIDTH, HEIGHT, REFRESH, c.connector_id);
			goto next;
		}
		out->mode = modes[j];
		e.encoder_id = encs[0];
		xioctl(DRM_IOCTL_MODE_GETENCODER, &e, "GETENCODER");
		for (j = 0; j < res.count_crtcs; j++)
			if (e.possible_crtcs & (1u << j))
				break;
		if (j == res.count_crtcs)
			goto next;
		out->connector_id = c.connector_id;
		out->crtc_id = crtcs[j];
		found = true;
		printf("t6drm: using connector %u, CRTC %u, mode %s (%u kHz)\n",
		       out->connector_id, out->crtc_id, out->mode.name,
		       out->mode.clock);
next:
		free(modes);
		free(encs);
	}
	free(crtcs);
	free(conns);
	if (!found)
		die("no connected connector with %ux%u@%u", WIDTH, HEIGHT,
		    REFRESH);
}

/* --- Steps ------------------------------------------------------------- */

static unsigned int nsteps;

static void manifest_step(const char *what, const int rect[4], long before,
			  long after, const char *file)
{
	fprintf(manifest, "%s\n    {\"step\": %u, \"what\": \"%s\", ",
		nsteps ? "," : "", nsteps, what);
	if (rect)
		fprintf(manifest, "\"rect\": [%d, %d, %d, %d], ", rect[0],
			rect[1], rect[2], rect[3]);
	else
		fprintf(manifest, "\"rect\": null, ");
	fprintf(manifest, "\"frames_before\": %ld, \"frames_after\": %ld, \"expected\": \"%s\"}",
		before, after, file);
	fflush(manifest);
	nsteps++;
}

static void cursor2(uint32_t crtc, uint32_t flags, uint32_t handle,
		    int x, int y, const char *what)
{
	struct drm_mode_cursor2 c = {
		.flags = flags,
		.crtc_id = crtc,
		.x = x,
		.y = y,
		.width = handle ? CURSOR_SIZE : 0,
		.height = handle ? CURSOR_SIZE : 0,
		.handle = handle,
	};

	int ret, err = 0;

	/* A failure here is recorded, not fatal, so later steps still run. */
	do {
		ret = ioctl(drm_fd, DRM_IOCTL_MODE_CURSOR2, &c);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		err = errno;
		failures++;
	}
	printf("t6drm: cursor: %s (%d, %d): %s\n", what, x, y,
	       err ? strerror(err) : "ok");
	fprintf(manifest, "%s\n    {\"cursor\": \"%s\", \"x\": %d, \"y\": %d, \"error\": %d}",
		nsteps++ ? "," : "", what, x, y, err);
	fflush(manifest);
	/* Cursor requests are synchronous; give the log a moment anyway. */
	sleep_ms(200);
}

int main(int argc, char **argv)
{
	/* x, y, w, h of each update; the last one ends at the corner. */
	static const int rects[][4] = {
		{ 96, 160, 512, 256 },
		{ 1000, 500, 300, 200 },
		{ 777, 333, 37, 19 },		/* tiny, unaligned */
		{ 0, 0, WIDTH, 40 },		/* full-width strip */
		{ WIDTH - 360, HEIGHT - 200, 360, 200 },
	};
	static const struct option opts[] = {
		{ "outdir", required_argument, NULL, 'o' },
		{ "status", required_argument, NULL, 's' },
		{ "settle-ms", required_argument, NULL, 'S' },
		{ "driver", required_argument, NULL, 'd' },
		{ }
	};
	const char *driver = "mct_t6";
	struct drm_mode_fb_cmd2 fb = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	struct dumb scr, cur;
	struct output out = { 0 };
	char path[4096], name[64];
	unsigned int i, x, y;
	long before, after;
	int c;

	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'o': outdir = optarg; break;
		case 's': status_path = optarg; break;
		case 'S': settle_ms = atoi(optarg); break;
		case 'd': driver = optarg; break;
		default:
			fprintf(stderr, "usage: t6drm --outdir DIR --status FILE [--settle-ms N]\n");
			return 2;
		}
	}
	if (!outdir || !status_path) {
		fprintf(stderr, "usage: t6drm --outdir DIR --status FILE [--settle-ms N]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	drm_fd = open_card(driver);
	if (drm_fd < 0)
		die("no DRM device for driver %s", driver);
	find_output(&out);

	snprintf(path, sizeof(path), "%s/manifest.json", outdir);
	manifest = fopen(path, "w");
	if (!manifest)
		die("create %s: %s", path, strerror(errno));
	fprintf(manifest, "{\n  \"width\": %d, \"height\": %d, \"mode\": \"%s\", \"clock\": %u,\n  \"steps\": [",
		WIDTH, HEIGHT, out.mode.name, out.mode.clock);

	/* Full screen: pattern, framebuffer, mode set. */
	dumb_create(&scr, WIDTH, HEIGHT);
	fill_rect(&scr, 0, 0, WIDTH, HEIGHT, 0);
	fb.width = WIDTH;
	fb.height = HEIGHT;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = scr.handle;
	fb.pitches[0] = scr.pitch;
	xioctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2");
	scr.fb_id = fb.fb_id;

	before = emu_frames();
	crtc.crtc_id = out.crtc_id;
	crtc.fb_id = scr.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&out.connector_id;
	crtc.count_connectors = 1;
	crtc.mode = out.mode;
	crtc.mode_valid = 1;
	xioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC");
	printf("t6drm: SETCRTC %ux%u fb %u\n", WIDTH, HEIGHT, scr.fb_id);
	after = wait_frames(before);
	save_expected(&scr, "expected-00.rgb");
	manifest_step("setcrtc", NULL, before, after, "expected-00.rgb");
	printf("t6drm: mode set produced frames %ld..%ld\n", before + 1, after);

	/* Partial updates through DIRTYFB. */
	for (i = 0; i < ARRAY_SIZE(rects); i++) {
		struct drm_clip_rect clip = {
			.x1 = rects[i][0],
			.y1 = rects[i][1],
			.x2 = rects[i][0] + rects[i][2],
			.y2 = rects[i][1] + rects[i][3],
		};
		struct drm_mode_fb_dirty_cmd dirty = {
			.fb_id = scr.fb_id,
			.num_clips = 1,
			.clips_ptr = (uintptr_t)&clip,
		};

		fill_rect(&scr, rects[i][0], rects[i][1], rects[i][2],
			  rects[i][3], i + 1);
		before = emu_frames();
		xioctl(DRM_IOCTL_MODE_DIRTYFB, &dirty, "DIRTYFB");
		after = wait_frames(before);
		snprintf(name, sizeof(name), "expected-%02u.rgb", i + 1);
		save_expected(&scr, name);
		manifest_step("dirtyfb", rects[i], before, after, name);
		printf("t6drm: DIRTYFB (%d,%d %dx%d) produced frames %ld..%ld\n",
		       rects[i][0], rects[i][1], rects[i][2], rects[i][3],
		       before + 1, after);
	}

	/*
	 * Burst: draw, flush, and flush the same rectangle again the moment
	 * the first frame lands.  With lossless refinement the first frame is
	 * raw (after a quiet spell), the second must be a JPEG whose row cache
	 * includes the rows that went out raw, and a raw refinement follows.
	 * Every frame shows the same, final image.
	 */
	{
		static const int burst[4] = { 300, 700, 400, 120 };
		struct drm_clip_rect clip = {
			.x1 = burst[0],
			.y1 = burst[1],
			.x2 = burst[0] + burst[2],
			.y2 = burst[1] + burst[3],
		};
		struct drm_mode_fb_dirty_cmd dirty = {
			.fb_id = scr.fb_id,
			.num_clips = 1,
			.clips_ptr = (uintptr_t)&clip,
		};

		fill_rect(&scr, burst[0], burst[1], burst[2], burst[3],
			  ARRAY_SIZE(rects) + 1);
		before = emu_frames();
		xioctl(DRM_IOCTL_MODE_DIRTYFB, &dirty, "DIRTYFB");
		wait_first_frame(before);
		xioctl(DRM_IOCTL_MODE_DIRTYFB, &dirty, "DIRTYFB");
		after = wait_frames(before);
		snprintf(name, sizeof(name), "expected-%02u.rgb",
			 (unsigned int)ARRAY_SIZE(rects) + 1);
		save_expected(&scr, name);
		manifest_step("burst", burst, before, after, name);
		printf("t6drm: burst DIRTYFB x2 (%d,%d %dx%d) produced frames %ld..%ld\n",
		       burst[0], burst[1], burst[2], burst[3], before + 1,
		       after);
	}

	/* Hardware cursor: a 64x64 ARGB shape with a soft-edged disc. */
	dumb_create(&cur, CURSOR_SIZE, CURSOR_SIZE);
	for (y = 0; y < CURSOR_SIZE; y++)
		for (x = 0; x < CURSOR_SIZE; x++) {
			int dx = (int)x - 32, dy = (int)y - 32;
			uint32_t a = dx * dx + dy * dy < 28 * 28 ? 0xff :
				     dx * dx + dy * dy < 31 * 31 ? 0x80 : 0;

			cur.map[y * (cur.pitch / 4) + x] =
				a << 24 | (x * 4) << 16 | (y * 4) << 8 | 0x40;
		}
	cursor2(out.crtc_id, DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
		cur.handle, 100, 100, "set");
	cursor2(out.crtc_id, DRM_MODE_CURSOR_MOVE, 0, 900, 600, "move");
	cursor2(out.crtc_id, DRM_MODE_CURSOR_MOVE, 0, -20, -10, "move");
	cursor2(out.crtc_id, DRM_MODE_CURSOR_BO, 0, 0, 0, "hide");

	/* Turn the output off again, and clean up. */
	before = emu_frames();
	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = out.crtc_id;
	xioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC off");
	printf("t6drm: CRTC disabled\n");
	fprintf(manifest, ",\n    {\"disable\": true, \"frames_before\": %ld}\n  ]\n}\n",
		before);
	fclose(manifest);

	xioctl(DRM_IOCTL_MODE_RMFB, &scr.fb_id, "RMFB");
	dumb_destroy(&cur);
	dumb_destroy(&scr);
	close(drm_fd);
	printf("t6drm: done, %u steps, %u failure(s)\n", nsteps, failures);
	return failures ? 1 : 0;
}
