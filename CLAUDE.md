# mct_t6: project notes for Claude

Linux DRM/KMS driver (C, out-of-tree, DKMS) for USB docks built on the **MCT
Trigger 6** chip, e.g. a j5create dock with HDMI + VGA. Start with README.md;
then see docs/PROTOCOL.md (wire protocol, sourced), docs/DESIGN.md (architecture
and why) and docs/TESTING.md (test layers and the hardware checklist).

## Hard rules

- **Never write to git**: no commit, push, add, stash, reset or checkout of
  files. The user manages the private remote. Read-only git is fine. Tell
  subagents this explicitly.
- **Hardware: reading is fine, changing state needs the user's OK.** Reading
  is `lsusb`, `dmesg`, sysfs, debugfs. Changing state means `insmod`/`modprobe`/
  `rmmod`, binding/unbinding, writing sysfs, suspend, or usbmon capture
  (needs root). Ask before each first-of-a-kind action; on a live USB a bad
  module can hang the session.
- On the original WSL machine the dock was attached to the *Windows* host and
  WSL could not see it (Windows-side queries only; no sudo there; tools came
  from `apt-get download` + `dpkg-deb -x`, see the `kernel-build-env` skill).
- Don't reinvent things that have a standard tool. The in-kernel JPEG encoder
  is the deliberate exception (there is no kernel JPEG library); see
  DESIGN.md.

## Resume here: status and next step

Development first happened on WSL without hardware. Done there:

- protocol research (docs/PROTOCOL.md, docs/research/);
- the driver (`src/`);
- host tests, all passing;
- a compile matrix clean on 6.1, 6.12, 6.18 and 7.2 (`W=1`, sparse);
- DKMS and Arch packaging, and docs;
- a UML end-to-end test in `tests/e2e/`: the real module against an emulated
  chip. It **passes**: 0 failures, KUnit 5/5, no KASAN or lockdep reports,
  fence and no-fence passes, and unplug/re-probe. It found 5 driver bugs, all
  fixed in `src/`: the CRTC-disable event was never sent, which led to a
  use-after-free on disconnect; `min_width` blocked cursor framebuffers;
  cursor moves re-sent whole frames; the cursor shape was re-uploaded on
  every move; and the code used the deprecated `system_unbound_wq`.

**Hardware bring-up started 2026-10-14** on a Lenovo 82QD laptop, Fedora 44
KDE live USB, kernel 6.19.10. The per-machine log and debugging playbook are
in `.claude/skills/hardware-bringup/{machines,debugging}.md`: **read them
first.** Working: discovery, HDMI and VGA (EDID, modes, picture), the JPEG
stream with reported fences, dock hot-unplug and replug, and the hardware
cursor. Two cursor bugs found on hardware are fixed: clipped shapes at screen
edges, and shape tearing (now double-buffered in chip slots 0/1). The unplug
`WARNING drm_mode_config.c:578` is a 6.19.10 DRM core issue, not ours.
User-facing install guide: docs/INSTALL.md.

Next, on hardware:
- A SuperSpeed port. So far the dock has only been on a USB 2-only laptop port;
  the 10 Gbps port held the live USB.
- The throughput test with video (checklist step 8), fbcon on a text VT,
  modetest, suspend/resume, and cursor alpha (premultiplied or not).
- Re-run the full compile matrix and the e2e test on the WSL machine. Both need
  kernel trees that aren't available on the live USB. The e2e emulator now
  rejects cursor slots > 9, and should gain a shape-change step that checks
  the slot alternation.

To start a session on the live USB:

1. Clone this repo (the private remote needs the user's SSH key on the live
   session).
2. Make sure `sudo -n true` works (see debugging.md), then run
   `tools/fedora-live-setup.sh`. It installs the toolchain and the matching
   `kernel-devel`, builds the module, runs the host tests, and checks Secure
   Boot and that the dock is present. Re-run it after every live boot.
3. Follow the `hardware-bringup` skill / docs/TESTING.md checklist. Record
   protocol findings in docs/PROTOCOL.md as **[HW]**, and setup findings in
   machines.md.

Live-USB gotchas:

- Secure Boot blocks unsigned modules: disable it in the firmware for testing.
- Writable space is RAM; don't build kernels there.
- Nothing outside the repo persists, so keep notes in the repo; the user
  commits them.

## Facts that took effort to establish

- The real USB device is **0711:5601** "T6 USB Station". 0711:5621 (display)
  and 0711:5640 (audio) are *virtual* children created by the Windows bus
  driver `t6sta.sys`; they never appear on the wire.
- The dock is USB 3: its hub and Ethernet report bcdUSB 2.10. But the laptop
  port used so far (HS01 / `usb3-port1`) is **USB 2.0 only**, so everything
  ran at 480M. **[HW] 2026-11-09:** on a direct USB-C connection the T6
  enumerates at 5000M (bcdUSB 3.20); a USB 2-only extension cable had forced
  480M. JPEG is mandatory below SuperSpeed (MCT's driver does the same); on
  SuperSpeed the driver adds lossless raw RGB24 frames (PROTOCOL.md §3.3).
- Cursor shapes: slots 0–9 per head. Showing a slot replaces the shown one;
  the driver double-buffers 0/1.
- The best protocol source is MCT's GPL reference driver
  https://github.com/mcttrigger/triggerdm (`t6usbdongle.c`, `main.c`, `t6.h`,
  `t6bulkdef.h`). The Windows usbmon captures are in
  https://github.com/cyrozap/mct-usb-display-adapter-re (captures/*.pcapng.gz).
- The Windows display driver is a UMDF IddCx driver: `t6indisp.dll` in
  DriverStore `trgidd64.inf_*`, with Intel IPP JPEG. Its Windows JPEGs use
  DRI=1 and component IDs 0/1/2. Ours use no restart markers, like triggerdm.
- Kernel API cutoffs, each verified against that release's headers (see the
  `kernel-api-compat` skill): hrtimer_setup 6.13, `drm_driver.date` removed
  6.14, `drm_dev_set_dma_dev` 6.16, `drm_edid_connector_add_modes` 6.3, const
  connector `mode_valid` 6.15, `usb_driver.shutdown` 6.11, fbdev setup
  variants 6.2/6.11/6.13/6.14, **`drm_atomic_state` renamed to
  `drm_atomic_commit` in 7.2** (handled with a `#define` in t6_compat.h).
- Ubuntu kernels build with `CONFIG_FRAME_WARN=1024` (and CI uses
  `-Werror`); keep every function's stack frame under 1 KiB. The JPEG
  encoder stages luma in the caller's s16 output for this reason.
  `tools/build-matrix.sh` passes `-Wframe-larger-than=1024`.
- sparse 0.6.4 can't parse `__has_include`. Use LINUX_VERSION_CODE checks in
  t6_compat.h.
- `shadow_plane_state->data[0]` already includes `fb->offsets[0]`. Don't add
  it twice.
- An object file can belong to only one kernel module, so `t6_kunit.c`
  `#include`s `t6_proto.c` and `t6_jpeg.c`.
- DRM pitfalls found by the e2e test:
  - `atomic_flush` isn't called for a CRTC being disabled, so send its event
    in `atomic_disable`.
  - `mode_config.min_width/height` apply to *all* framebuffers, cursors
    included.
  - A plane update without damage clips counts as full damage.
- UML needs `seccomp=on` for SMP, and `UML_PCI_OVER_VIRTIO` for the
  IOMEM/DMA emulation that USB and DRM require.

## Layout and conventions

- `src/`: kernel code, kernel coding style, SPDX GPL-2.0-only.
  `t6_proto.c` and `t6_jpeg.c` are pure (no kernel APIs, via `t6_types.h`),
  so they are also built by the host tests. Keep them that way.
- Every kernel-version `#if` lives in `src/t6_compat.h`, nowhere else.
- `tests/host/`: ASan/UBSan unit tests. Python checks need Pillow + numpy;
  run `python -I`.
- `tests/e2e/`: User-Mode Linux harness: dummy_hcd + raw_gadget chip emulator
  (`t6emu`), DRM client (`t6drm`), `verify.py`.
- `tools/build-matrix.sh`: compile test across kernel trees (`W=1`, sparse).
- `tools/fedora-live-setup.sh`: prepare a Fedora (live) session for hardware work.
- `tools/release.sh` (via `make version`, `make release-check`, `make release
  RELEASE=x.y.z`, `make dist`): versioning and tagging. The version lives in
  `dkms.conf`. `make release` commits and tags, so only the user runs it.
  `.github/workflows/release.yml` builds on tag push and publishes the release.
- `docs/research/` + `tools/research/`: raw protocol research (decoded Windows
  captures, prior-driver analysis) and the stdlib-Python pcapng/usbmon parsers
  that produced it. These also work on Linux usbmon captures, for diffing our
  traffic against Windows'.

## Skills (.claude/skills/)

- `kernel-build-env`: kernel trees and build tools without sudo.
- `kernel-api-compat`: verifying and shimming kernel API changes; table of known cutoffs.
- `t6-protocol-research`: where protocol facts come from and how to dig further.
- `hardware-bringup`: real-dock testing and usbmon debugging.
- `e2e-uml`: running and extending the emulated-chip end-to-end test.

## Commands

```sh
make check PYTHON="<venv>/bin/python -I"     # host tests
tools/build-matrix.sh <kdir>...              # compile matrix
tests/e2e/run.sh                             # end-to-end in UML
```

Before calling a change done: host tests pass, and the matrix is clean on
6.1, 6.12, 6.18 and 7.2.
