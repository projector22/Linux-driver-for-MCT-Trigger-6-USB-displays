# Design

## Goals and constraints

- Work as a normal monitor on any desktop (Wayland or X11), on Fedora, Debian
  and Arch, with as few dependencies as possible.
- At least 60 fps at 1080p over the link these docks actually get. On the
  development laptop that is **USB 2.0 High Speed**: the dock sits behind a USB
  2.0 hub on a high-speed root port.
- Production quality: clean teardown on unplug, suspend/resume, no busy loops,
  bounded memory.

## Architecture: an in-kernel DRM driver

There were two realistic designs.

1. **Kernel DRM driver (chosen).** The driver registers a DRM device, so the
   compositor renders into it like any GPU output and the driver turns damage
   into USB transfers. This follows the in-tree `gud`, `udl` and `gm12u320`
   drivers.
2. **Userspace daemon on EVDI.** This is MCT's own Linux driver (`triggerdm`)
   and DisplayLink's design: the out-of-tree `evdi` module provides a virtual
   DRM output, and a daemon grabs frames, compresses them with libjpeg-turbo and
   sends them with libusb.

Option 2 can use libjpeg-turbo's SIMD encoder, but it needs *three* external
pieces: the evdi DKMS module, which isn't in Fedora's or Arch's main
repositories, plus libevdi and a privileged daemon managed by udev/systemd.
Option 1 is a single DKMS module with no runtime dependencies, works from the
first boot message (fbdev console), and gets lifetime, hot-plug and power
management from the kernel's USB and DRM cores.

The price of option 1 is that Linux has no software JPEG encoder, so the driver
carries one (`t6_jpeg.c`, ~500 lines of integer C). The algorithms are textbook
ITU-T T.81 with the IJG integer DCT, and the code is tested against libjpeg (see
TESTING.md). This is the one place the driver implements something a library
would normally provide; there is no in-kernel library to reuse.

## Source layout

| File | Responsibility |
|---|---|
| `t6_drv.c` | USB probe/disconnect/PM, device discovery, module parameters, debugfs |
| `t6_usb.c` | control requests, bulk objects, interrupt endpoint, frame-done fences |
| `t6_kms.c` | planes, CRTCs, encoders, connectors; mode filtering; vblank timer |
| `t6_cursor.c` | hardware cursor plane |
| `t6_stream.c` | frame pipeline: damage → row encode → assemble → send |
| `t6_proto.c` | wire formats (pure, host-tested) |
| `t6_jpeg.c` | JPEG encoder (pure, host-tested) |
| `t6_compat.h` | every kernel-version difference, 6.1 to 7.2 |
| `t6_types.h` | lets the pure files build in-kernel and on the host |

## Frame pipeline

```
compositor commit
  └─ primary plane atomic_update  (t6_kms.c)
       for each damage clip: copy pixels → stream->shadow, mark MCU rows dirty
       kick the stream worker
                                    stream worker (t6_stream.c, unbound wq)
                                      snapshot dirty rows → stream->pixels
                                      encode those rows in parallel  (t6_jpeg.c)
                                      assemble JPEG from the row cache
                                      wait for fences (≤ 2 frames in flight)
                                      place in VRAM ring, send       (t6_usb.c)
```

**Why rows.** A JPEG scan without restart markers is one serial bitstream, so
changing one pixel normally means re-encoding everything. The encoder instead
keeps one entry per 16-line MCU row, holding the row's Huffman-coded bits
(minus its first MCU) plus the first MCU's coefficients and the DC predictors
the row ends with. Assembling a frame re-codes each row's first MCU against the
real predictor and splices the cached bits in at bit granularity. The output is
byte-identical to a one-pass encode (tested), but typing in a terminal only
re-encodes the one or two rows the text is on. Restart markers would make rows
independent more simply, but they cost bandwidth and are a less exercised
decoder feature. MCT's reference driver doesn't use them either.

**Parallelism.** Dirty rows are split evenly over up to `encode_jobs` work
items on the driver's own unbound workqueue. A full 1080p frame takes ~18 ms on one
core in the host benchmark, so a few cores give ample headroom for 60 fps;
desktop updates are typically a few rows.

**Coalescing.** Commits never wait for the encoder. If frames arrive faster
than the link takes them, damage accumulates in the dirty bitmap and the next
frame carries all of it, so latency stays bounded and nothing queues.

**Flow control.** Every video object carries a fence number that the chip
echoes on its interrupt endpoint when done. The worker keeps at most two
frames per head in flight. If the chip never reports fences, the worker
doesn't wait at all, as MCT's driver doesn't.

**Adaptive quality.** The per-frame byte budget is ~35 MB/s ÷ 60 on High Speed.
Three consecutive frames over budget lower the quality by 5 (minimum 30).
120 frames under half the budget raise it back towards `jpeg_quality`. Changing
quality invalidates the row cache, so it changes deliberately slowly. A frame
too big for a 3 MiB ring object is re-encoded at once at lower quality.

**Lossless refinement.** JPEG at any quality keeps 4:2:0 chroma and
ringing around text. With `lossless` on (by default on SuperSpeed), once a
head has been quiet for `lossless_delay_ms` the worker sends the whole frame
as raw RGB24 (format 9, header immediately before the pixels, as the Windows
JCD543 driver does) into one of three raw framebuffers carved off the top of
the JPEG ring; the ring keeps at least 8 MiB. An update arriving after a quiet
spell goes out raw at once; rapid updates stay JPEG and schedule a raw frame
for when they stop. Quiet time is measured between damage arrivals, not from
frame sends, so a link where a raw frame takes longer than the delay (USB 2
with `lossless=1`) still falls back to JPEG during motion. Rows sent raw are marked stale in the JPEG row cache and
re-encoded before the next JPEG. Raw objects exceed the URB pool, so
`t6_send_object()` sends them in waves under one bulk header. Three raw
failures in a row turn lossless off for the head until the next mode set.
**Best picture first.** Rapid updates go out raw as well, for as long as raw
frames still reach `lossless_min_fps` (default 20). Each raw frame is timed
from conversion to the end of its transfer, fence wait included, and the
times are averaged over about four frames (`t6_raw_cost_update()`). Above the
frame period, motion falls back to JPEG with refinement. It returns to raw
only once the average is below 7/8 of the period, so a cost near the limit
doesn't flip on every frame (`t6_raw_motion_next()`). While motion is JPEG,
the refinement frames keep the average current. Then, if JPEG frames still
overrun the link, quality control lowers JPEG quality. The order is: raw
always, then JPEG in motion with raw refinement, then lower JPEG quality.
At ~36 ms per 1080p frame on SuperSpeed (~27 fps), one head stays raw
throughout. Two heads, or USB 2 with `lossless=1`, fall back. debugfs
`stats` shows `motion raw|jpeg` and `raw_cost_ms`. `lossless_always` skips
both tests, so every frame is raw however slow.

**Failures.** A failed transfer resynchronises the chip's bulk parser (vendor
request `0x14`, as MCT's driver does), marks the frame's rows dirty again and
retries after 100 ms.

## KMS model

- One primary plane (XRGB8888/ARGB8888, linear), one cursor plane, CRTC,
  encoder and connector per head. Heads and connector types (HDMI/VGA/DVI) come
  from the chip's display section header.
- **Modes** are limited to the chip's own timing table, because its PLL values
  depend on the reference clock. EDID modes without a table entry are rejected
  in `mode_valid`. If no EDID mode is usable (no EDID, or a 4K-only monitor),
  the table's modes up to 1080p are offered instead. Modes must also fit a
  4 MiB NV12 framebuffer slot.
- **Vblank** is simulated with an hrtimer at the mode's refresh rate, as `vkms`
  does, so compositors pace themselves to 60 Hz instead of spinning.
- `commit_tail_rpm` ordering enables CRTCs (mode set + stream start) before
  planes are updated, so the first damage lands in a running stream.
- **Shadow-plane helpers** give a CPU mapping of each framebuffer.
  `drm_gem_fb_begin/end_cpu_access` brackets reads of imported dma-bufs.

## Hardware cursor

DRM cursor plane, ARGB8888, up to 128×128 (advertised as 64×64). The shape is
uploaded only when the framebuffer or its content changes. Moves are a single
`0x04`/`0x06` control request. On a USB 2.0 link this is the difference between
a smooth pointer and one that lags behind frame encoding.

## USB transport

- Control requests go through a kmalloc'd bounce buffer (USB buffers must be
  DMA-able), serialised by `ctrl_lock`.
- Bulk objects are a 32-byte header plus payload. The payload is copied into up
  to 24 pre-allocated 128 KiB URBs, which are all submitted together on a
  `usb_anchor` and waited for with a timeout, so the host controller streams
  without gaps. `bulk_lock` keeps header and payload of one object adjacent
  when both heads are active.
- The interrupt URB is always submitted, as in MCT's drivers. An experimental
  driver that never read it saw unexplained disconnects. That this was the
  cause is a hypothesis, but draining the endpoint is the vendor's behaviour
  anyway.
- Only display-signature (0) bulk objects can be generated; the firmware-flash
  signature is unreachable by construction.

## Lifetime and locking

- `devm_drm_dev_alloc` ties the DRM device to the USB interface. Every hardware
  access is inside `drm_dev_enter/exit`, so after `drm_dev_unplug()` nothing
  touches the device.
- Disconnect order: stop the connector poller → unplug DRM → shut down atomic
  state (disables CRTCs, stops streams) → cancel vblank timers → kill URBs and
  hot-plug work → destroy the workqueue → free buffers.
- Lock order: `stream->lock` (never held across I/O) is independent of
  `bulk_lock` → `ctrl_lock`. The vblank timer callback takes no driver locks and
  stops re-arming once `vblank_enabled` is cleared.
- Suspend/resume use `drm_mode_config_helper_suspend/resume`. Re-enabling the
  CRTCs re-sends the mode, which fully re-initialises the chip.

## Kernel compatibility

`t6_compat.h` covers every API difference from 6.1 (Debian 12) to 7.2 (Arch):

- fbdev client setup (four variants);
- `drm_driver.date`;
- the DMA device for PRIME;
- EDID mode helpers;
- `hrtimer_setup`;
- the const connector `mode_valid`;
- `usb_driver.shutdown`;
- 7.2's rename of `drm_atomic_state` to `drm_atomic_commit`.

Each version boundary was checked against that release's headers.
`tools/build-matrix.sh` compile-tests real trees with `W=1` and sparse.
