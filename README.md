# mct_t6: Linux driver for MCT Trigger 6 USB displays

A Linux DRM/KMS driver for USB display adapters and docks built on the Magic
Control Technology (MCT) **Trigger 6** chip. Examples are j5create docks such
as the JCD543, with HDMI and VGA outputs alongside USB-A ports, Ethernet and
audio. On such docks the Ethernet and USB ports already work on Linux; the
display outputs need this driver.

The driver registers a normal DRM device, so GNOME, KDE, Sway and X11 see the
dock's outputs as ordinary monitors: hot-plug, resolution changes, a hardware
cursor and a text console all work through the standard interfaces.

> **Status: early testing on hardware.** It builds warning-free on kernels
> 6.1 through 7.2 and passes its host tests, KUnit, and an end-to-end test
> against an emulated chip under KASAN and lockdep. On a real j5create HDMI+VGA
> dock (Fedora 44, kernel 6.19, KDE), both outputs, the JPEG stream and dock
> hot-plug and the hardware cursor work. It also installs and drives the dock
> on Ubuntu with kernel 7.0 and Secure Boot on, via DKMS and an enrolled MOK
> key. See [docs/TESTING.md](docs/TESTING.md) for the bring-up checklist.

## How it works

| | |
|---|---|
| USB device | The dock exposes one vendor-specific device, `0711:5601` "T6 USB Station" (driver matches `0711:5600–561F` and `5650–565F`, as MCT's Windows driver does). |
| Transport | Frames are sent as **baseline JPEG** and decoded by the chip. Over USB 2.0 that is the only way to reach 60 fps: raw 1080p60 would need ~10× the link's bandwidth. MCT's own drivers do the same. |
| Encoding | An in-kernel, integer-only JPEG encoder that re-encodes only the 16-line strips that changed, spread across CPU cores (see [docs/DESIGN.md](docs/DESIGN.md)). |
| Pacing | Frames are sent only when the screen changes. A timer-driven vblank paces compositors at the mode's refresh rate. JPEG quality adapts to the link. |
| Cursor | Uses the chip's hardware cursor, so moving the pointer costs one tiny USB request instead of a frame. |
| Modes | Up to 1920×1200 / 2048×1152 per output at the refresh rates in the chip's mode table (60 Hz at 1080p; up to 75 Hz at lower resolutions). |

The protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md), with the
source of every fact.

## Requirements

- Linux **6.1 or newer** with DRM, USB and the DRM shmem helpers enabled; all
  stock Fedora, Debian/Ubuntu and Arch kernels qualify.
- Kernel headers for the running kernel, and DKMS.

There are no runtime dependencies: no daemon, no userspace libraries.

## Installation

Full instructions for **Fedora**, **Debian / Ubuntu** and **Arch**, with DKMS
or a manual build, plus Secure Boot, updating and troubleshooting, are in
**[docs/INSTALL.md](docs/INSTALL.md)**. The quick versions below install with
DKMS, so the module is rebuilt for every new kernel. Run them from a checkout
of this repository.

**Debian / Ubuntu** (and Mint, Pop!_OS, …):

```sh
sudo apt update
sudo apt install build-essential dkms # linux-headers-$(uname -r)
make clean && sudo mkdir -p /usr/src/mct-t6-0.2.2
sudo cp -r dkms.conf src /usr/src/mct-t6-0.2.2/
sudo dkms install mct-t6/0.2.2
```

If Secure Boot is on (`mokutil --sb-state`), enroll Ubuntu's DKMS signing key
once, or the module builds but never loads:

```sh
mokutil --test-key /var/lib/shim-signed/mok/MOK.der   # "is not enrolled"? then:
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der  # set a one-time password
sudo reboot   # blue MOK screen: Enroll MOK → Continue → Yes → password → Reboot
```

Ubuntu 22.04 needs the HWE kernel (`linux-generic-hwe-22.04`); its stock 5.15
kernel is too old.

**Fedora** (and RHEL, Alma, Nobara, …):

```sh
sudo dnf install gcc make elfutils-libelf-devel dkms kernel-devel-$(uname -r)
make clean && sudo mkdir -p /usr/src/mct-t6-0.2.2
sudo cp -r dkms.conf src /usr/src/mct-t6-0.2.2/
sudo dkms install mct-t6/0.2.2
```

**Arch** (and Manjaro, EndeavourOS, …), as a pacman package:

```sh
sudo pacman -S --needed base-devel dkms linux-headers   # or linux-lts-headers, linux-zen-headers, …
make clean
cd packaging/arch && makepkg -si
```

With Secure Boot on, the module must be signed with an enrolled key or it
won't load; see [Secure Boot](docs/INSTALL.md#secure-boot).

Only one driver can own the dock. If you previously installed another
Trigger 6 driver (for example a DKMS `trigger6` module), remove it first:

```sh
dkms status                                   # look for trigger6 or similar
sudo dkms remove trigger6/<version> --all
sudo rm -f /etc/modprobe.d/trigger6.conf
```

Then plug in the dock. The module loads automatically through its USB device
table, and the outputs show up in your display settings.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `jpeg_quality` | 85 | Maximum JPEG quality (30–100). Lowered automatically while the link is saturated, then raised again. |
| `encode_jobs` | 0 | Parallel encoder jobs per frame; 0 = one per CPU (max 8). |
| `hw_cursor` | true | Use the chip's hardware cursor. |
| `lossless` | -1 | Once the screen has been still for `lossless_delay_ms`, resend it as a lossless raw RGB frame, which makes text pixel-exact: `1` on any link, `0` never (JPEG only), `-1` on SuperSpeed (USB 3) links. |
| `lossless_delay_ms` | 100 | Quiet time before the lossless frame, when motion is being sent as JPEG. |
| `lossless_min_fps` | 20 | Wherever `lossless` applies, moving content (scrolling, video) is sent lossless too, as long as lossless frames still reach this rate. If they can't, motion goes out as JPEG and the still picture is refined. JPEG quality then drops only if the link is still saturated. `0` sends motion as JPEG. |
| `lossless_always` | false | Wherever `lossless` applies, send every frame lossless, however slow. |
| `trust_damage` | -1 | Send only the rectangles userspace reports as changed: `1` always, `0` never (compare whole frames, more CPU), `-1` until userspace reports a rectangle outside the framebuffer, then compare frames until the next mode set. |

Set them persistently in `/etc/modprobe.d/mct_t6.conf`, e.g.
`options mct_t6 jpeg_quality=90`.

## Troubleshooting

- **Installed fine, but no display?** Check `lsmod | grep mct_t6`. If it's
  missing and `mokutil --sb-state` says Secure Boot is enabled, the signing key
  isn't enrolled: `dkms install` succeeds, but the kernel silently refuses the
  module. Enroll the key as shown under [Installation](#installation).
- If `lsmod` shows a different driver instead (e.g. `trigger6`), it claimed the
  dock first. Remove it as shown under [Installation](#installation), then
  replug the dock.
- `dmesg | grep mct_t6` shows what the driver found: output count, VRAM, firmware
  version and link speed.
- `/sys/kernel/debug/mct_t6-<usb-interface>/stats` (debugfs) shows per-output
  statistics: frames, bytes, current JPEG quality, encode and send time, and
  dropped frames.
- **Part of a rotated (portrait) monitor stays black on X11**, while the pointer
  still moves over it: X11's modesetting driver reports changed areas in the
  unrotated layout. The driver detects this and falls back to comparing frames
  (`dmesg` says `comparing frames instead`). If an area still never updates,
  set `options mct_t6 trust_damage=0`, or use a Wayland session.
- A VGA monitor without working DDC can't be detected; force it on with the
  kernel parameter `video=VGA-1:e`.
- If the link is USB 2.0, the log says `high-speed`. A SuperSpeed port gives far
  more headroom.

## Development

```sh
make check                               # host unit tests (needs python3 + Pillow + numpy)
tools/build-matrix.sh <kdir> [<kdir>...] # compile-test against kernel trees (W=1, sparse)
tests/e2e/run.sh                         # full driver vs. emulated chip under User-Mode Linux
```

See [docs/TESTING.md](docs/TESTING.md) for all test layers, and
[docs/DESIGN.md](docs/DESIGN.md) for the architecture.

### Releasing

The version lives in `dkms.conf`; `packaging/arch/PKGBUILD` and the install
commands in the docs must match it (`make release-check` verifies this).

```sh
make release RELEASE=0.2.0          # bump the version everywhere, commit, tag v0.2.0
git push --atomic origin main v0.2.0
```

Pushing a `v*.*.*` tag runs `.github/workflows/release.yml`: it checks the tag
matches `dkms.conf`, runs the host tests, builds the module with DKMS on
Debian, Ubuntu, Fedora and Arch, and publishes a GitHub release with the
source tarball (`make dist`). The workflow can also be started by hand from
the Actions tab to test a branch; that run doesn't publish anything.

## Limitations and roadmap

- Hardware validation so far covers one dock on one laptop, over USB 2.0 only:
  see docs/TESTING.md for what's still open.
- No 4K: the chip can do 3840×2160@30 over SuperSpeed with a different memory
  layout, which is not implemented.
- Each update sends a whole-frame JPEG. The Windows driver can also patch
  rectangles (protocol command 7), which would cut USB traffic for small
  changes.
- Audio (HDMI audio session 3) is not implemented.

## Credits

- MCT's GPL reference driver [triggerdm](https://github.com/mcttrigger/triggerdm).
- [cyrozap/mct-usb-display-adapter-re](https://github.com/cyrozap/mct-usb-display-adapter-re) for protocol notes and Windows captures.
- [RL-Charles/jcd543-mct-t6-linux-driver](https://github.com/RL-Charles/jcd543-mct-t6-linux-driver), whose test logs informed several design decisions.

## License

Copyright (C) 2026 Gareth Palmer

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License version 2 only, as published by
the Free Software Foundation. It is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE)
for the full text.

Each source file carries an `SPDX-License-Identifier: GPL-2.0-only` tag. The
licence matches the kernel's own and that of MCT's GPL-2.0 `triggerdm`
reference driver, which was used as a protocol source. No code was copied
from it.
