# End-to-end test: mct_t6 against an emulated Trigger 6

This test runs the real `mct_t6` driver inside a User-Mode Linux (UML) kernel. The
driver talks over `dummy_hcd` to a software Trigger 6 (`t6emu`, built on
raw-gadget), while a small KMS client (`t6drm`) sets a mode, draws frames and
moves the hardware cursor. Everything runs as an ordinary user. No real USB
hardware is involved: the host controller is the in-kernel dummy and
`UML_PCI_OVER_VFIO` is off.

```
 UML kernel (hostfs root = host /)
 ┌───────────────────────────────────────────────────────────────┐
 │ t6drm ──ioctl──▶ mct_t6 ──URBs──▶ dummy_hcd ═╗                │
 │                                              ║ (in-kernel)    │
 │ t6emu ◀──/dev/raw-gadget── raw_gadget ◀── dummy_udc ═╝        │
 └───────────────────────────────────────────────────────────────┘
        │ frames, log.jsonl, expected images, dmesg (via hostfs)
        ▼
   verify.py (host, Pillow + numpy)
```

## Running

```sh
T6_E2E_WORK=/path/with/4GB/free \
T6_BUILDENV=/path/to/buildenv.sh \
T6_PYTHON=/path/to/venv/bin/python \
tests/e2e/run.sh [--patch fix.diff]... [-j N]
```

`run.sh` does everything and exits non-zero on any failure:

1. It downloads `linux-7.2.10.tar.xz` from kernel.org and checks it against a
   pinned SHA-256. It extracts the tarball once, under `$T6_E2E_WORK/src`. The
   source tree is never written to, because all builds use `O=`.
2. It configures the UML kernel from `make ARCH=um defconfig` plus
   [`uml.config`](uml.config). It checks that every option in the fragment
   survived, then builds the kernel in `$T6_E2E_WORK/build/uml`.
3. It copies `src/` to `$T6_E2E_WORK/mod`, applies any `--patch` files to
   that copy only, and builds `mct_t6.ko` and the KUnit module
   `mct_t6_test.ko` (`CONFIG_MCT_T6_KUNIT_TEST=m`). `src/` itself is never
   modified and no build artefacts land in the repository.
4. It builds `t6emu` and `t6drm` statically with the host gcc.
5. It boots UML with the host filesystem as root
   (`rootfstype=hostfs rootflags=/`, `ncpus=2 seccomp=on`, `mem=1G`), with
   [`uml-init.sh`](uml-init.sh) as PID 1.
6. It runs [`verify.py`](verify.py) on the output.

A warm run, with the kernel already built, takes about 1–2 minutes. A cold
run spends about 10 minutes building the kernel. Disk use is about 2.3 GB:
1.8 GB of kernel source, about 400 MB of build output and about 80 MB of
results.

`--patch` is meant for trying a driver fix without touching `src/`. The patch
is a `-p1` diff with `a/src/...` paths.

### Requirements

- Linux x86-64 host, gcc with static glibc (`libc.a`), make, curl, xz, and
  `patch`.
- The usual kernel build dependencies: flex, bison, bc and libelf headers.
  Without root, put them in a private prefix and point `T6_BUILDENV` at a
  script that adds it to `PATH` and the include and library paths.
- `/dev/shm` on tmpfs. UML backs guest RAM with a file there.
- Python 3 with Pillow and numpy. `verify.py` is run with `-I`.
- 4 GB of free disk space and about 2 GB of RAM.

## What runs in the guest

`uml-init.sh` mounts proc, sys, devtmpfs and debugfs, then runs these steps:

1. **KUnit.** It loads `mct_t6_test.ko`, whose results land in dmesg as KTAP,
   then unloads it.
2. **Pass `fence`.** It starts `t6emu` and loads `mct_t6.ko`, which probes the
   emulated dock, then waits for the DRM device. Next it runs `t6drm` and
   copies `/sys/kernel/debug/mct_t6-*/stats`. Finally it stops `t6emu`
   (SIGTERM), which unplugs the device and exercises the driver's
   disconnect path.
3. **Pass `nofence`.** This is the same as `fence`, but with
   `t6emu --no-fence`. The emulator never reports frame completion, so this
   exercises the driver's fallback. The driver re-probes the new device
   without being reloaded.
4. **Passes `lossless` and `always`.** These are the same as `fence`, but
   with the `lossless=1` module parameter, and then also `lossless_always=1`.
   The emulator is a High Speed device, where lossless frames are off by
   default. The first pass exercises raw RGB24 refinement after a quiet
   spell. The second sends every frame raw.
5. **Cleanup.** It runs `rmmod mct_t6`, saves dmesg and powers off through
   sysrq.

Every step has a bounded timeout, and outcomes go to `init.log`.

## The pieces

### `t6emu.c`: Trigger 6 emulator

- **USB device.** It presents `0711:5601`, bcdUSB 2.00, High Speed. It has one
  interface (ff/00/00) with bulk IN, bulk OUT and a 64-byte interrupt IN
  endpoint. The endpoint addresses are chosen from `USB_RAW_IOCTL_EPS_INFO`.
  On `dummy_udc` they come out as 0x81, 0x02 and 0x85.
- **Vendor requests.** Requests follow [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md).
  The answers come from a real dock: [`t6emu_data.h`](t6emu_data.h) holds the
  0xB3 section header (head 0 DVO with 36 timings, head 1 DAC with 30), both
  timing tables and the 1080p monitor EDID. All of it comes verbatim from
  Windows-driver captures, and the timing tables were cross-checked against
  the raw 0x89 transfers. VRAM is 58 MiB and the platform is 1.
- **Connectors and EDID.** `--head0`/`--head1 connected|disconnected` set
  the connector status (by default head 0 is connected and head 1 is not). A
  connected head serves the EDID. On a disconnected head, the 0x80 EDID
  request is **stalled**, like a DDC read nobody answers.
- **OUT requests.** These are accepted, validated and logged. That covers
  monitor control, timing, software ready, cursor shape, position and state,
  and cancel-bulk. For example, the 0x12 record must be an entry of that
  head's table, and 0x03 on must follow 0x12 and 0x31. Requests the driver
  must never send (ROM writes 0x13/0x32, reset 0x30) and unknown requests are
  stalled and flagged.
- **Bulk OUT.** The emulator reads the 32-byte header and then exactly
  `payload_length` bytes. It checks:
  - signature 0, no fragmentation, reserved bytes zero;
  - the ring address is inside that head's ring and MiB aligned;
  - the command matches a head with a mode set and its output on;
  - formats 6 and 13;
  - pitches equal to `ALIGN(w,32)`;
  - the Y address is one of the three slots, and
    UV = Y + `ALIGN(w,32)*ALIGN(h,32)` + 1024;
  - `payload_size` = len − 48;
  - the payload is FFD8…FFD9 followed by 1024 zero bytes;
  - the SOF0 header says the mode size and 4:2:0;
  - the reset flag is set on the first 10 frames after a mode set, and
    otherwise only on a ring wrap;
  - fences increase.

  Each JPEG is saved as `frame-NNNNN.jpg`.
- **Interrupt IN.** After each video object, the emulator sends a frame-done
  packet: byte 0 = 0x04, the fence at 0x0C and byte 0x13 = 0x04. Pass
  `--no-fence` to suppress these packets.
- **Output.** Each event is written as one JSON line to `log.jsonl`. That
  includes control requests with their data in hex, decoded bulk objects,
  interrupt packets and USB bus events, each with an `errors` array.
  `emu.status` holds the running frame count.
- **Shutdown.** SIGTERM shuts the emulator down cleanly.

### `t6drm.c`: KMS client (raw ioctls, no libdrm)

1. It finds `/dev/dri/cardN` for the driver named `mct_t6`, plus the
   connected connector, and picks 1920x1080@60.
2. It draws a deterministic test pattern into a dumb buffer: colour bars,
   gradients, a checkerboard and fine diagonals. It adds the buffer with
   `ADDFB2` (XRGB8888) and calls `SETCRTC`.
3. It changes 5 rectangles and reports each with `DIRTYFB`. One rectangle is
   tiny and unaligned (37x19), one is a full-width strip, and one touches the
   bottom-right corner.
4. Burst: it changes one more rectangle, reports it with `DIRTYFB`, and
   reports the same rectangle again as soon as the first frame arrives.
   With lossless refinement this forces raw, then JPEG, then raw, all of
   the same image.
5. It exercises the cursor with `DRM_IOCTL_MODE_CURSOR2`, using a 64x64
   ARGB dumb buffer: set at (100,100), move to (900,600), move partly
   off-screen to (−20,−10), then hide.
6. It disables the CRTC.

After each screen change, `t6drm` waits until the emulator's frame count has
been stable for 1.5 s. It then writes the expected image as raw RGB24
(`expected-NN.rgb`). `manifest.json` records the rectangle and the emulator
frame range of each step.

### `verify.py`

| Area | Checks |
|---|---|
| Guest | `init.log` has no failures and reached the end |
| KUnit | Every `mct_t6` case is `ok`, the suite is `ok`, and the totals add up |
| dmesg | No `WARNING:`, BUG, Oops, KASAN, lockdep or RCU reports, hung tasks, `*ERROR*` lines, deprecation warnings or call traces |
| Protocol | No emulator validation errors in either pass |
| Mode set | Every mode set is 0x12 → 0x31 → 0x03 on, and the first one carries the exact 32-byte 1080p60 record from the table (read from `t6emu_data.h`) |
| Fences | Fences strictly increase. In the `fence` pass every one is completed; in the `nofence` pass none is |
| Frames | Every frame decodes at 1920x1080 |
| Raw frames | Header fields match PROTOCOL.md §3.3. A raw object never overlaps an NV12 slot or the surface on screen, and JPEG objects never overlap raw ones. Raw frames are pixel-exact against the expected (or previous) image. There are none in `fence`/`nofence`, every step ends raw in `lossless`, and every frame is raw in `always` |
| Images | See the PSNR paragraph below |
| Cursor | See the cursor paragraph below |
| Disable | Disabling the CRTC sends 0x03 off |
| Stats | debugfs frame, raw-frame and byte counts equal what the emulator received, the fences state is right, and there are no drops or fence timeouts |

For images, the last frame of each step must match the expected image, both
over the whole frame and inside the changed rectangle. Earlier frames of a
step must match either the new or the previous image. The rectangle must
also differ from the previous image, so a stale frame can't pass.

**PSNR thresholds** are calibrated rather than guessed. Each expected image
(and each rectangle) is compressed with libjpeg through Pillow at the
driver's settings, quality 85 with 4:2:0 subsampling. The frame must come
within 2 dB of that, with a sanity floor of 20 dB. The pattern is
deliberately hard for JPEG, so libjpeg itself reaches only about 28 dB on
it. mct_t6's encoder comes within 0.05 dB of libjpeg. The previous image
scores 5–9 dB, so the check discriminates well.

For the cursor, the first item must be a shape upload with header type 1,
64x64, pitch 256, sent in chunks of at most 4 KiB at contiguous offsets, and
the pixels must equal what `t6drm` drew. It is followed by: position
(100,100), show, (900,600), (−20,−10), hide. Cursor-only updates must not
send any video. Re-uploading an unchanged shape is reported as a warning.

## Files

| File | |
|---|---|
| `run.sh` | Single entry point |
| `uml.config` | UML kernel config fragment, including KASAN, PROVE_LOCKING and debug objects |
| `uml-init.sh` | Guest PID 1 |
| `t6emu.c`, `t6emu_data.h` | Emulator and captured device data |
| `t6drm.c` | KMS client |
| `verify.py` | Host-side checks |

Results go to `$T6_E2E_WORK/out`:

- `console.txt` and `dmesg.txt`;
- `init.log`;
- one directory per pass, holding `log.jsonl`, `frame-*.jpg`,
  `expected-*.rgb`, `manifest.json`, `stats.txt`, `t6emu.out` and
  `t6drm.out`.
