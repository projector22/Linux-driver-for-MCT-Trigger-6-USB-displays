# Building, installing and running mct_t6

This guide covers the three main distribution families: **Fedora**,
**Debian / Ubuntu** and **Arch**. Derivatives follow their parent: RHEL, Alma
and Nobara follow Fedora; Mint, Pop!_OS and elementary follow Ubuntu; Manjaro
and EndeavourOS follow Arch.

There are two ways to install:

| | When to use it |
|---|---|
| **DKMS** (recommended) | Everyday use. DKMS rebuilds the module for every new kernel, and it loads automatically when the dock is plugged in. |
| **Manual build + `insmod`** | Development, testing a change, or a live USB. Nothing is installed; the module is gone after a reboot. |

Requirements, whichever route you take:

- Linux **6.1 or newer**. Debian 12+, Ubuntu 24.04+ (22.04 only with the HWE
  kernel), current Fedora and Arch all qualify. `uname -r` shows your version.
- Headers for the **running** kernel. Most problems come from headers that
  don't match `uname -r`; the per-distro sections below show how to get the
  right ones.
- Secure Boot either disabled, or a key enrolled to sign the module. See
  [Secure Boot](#secure-boot).

The steps below assume you're in a checkout of this repository:

```sh
git clone <repo-url> usb-multidevice-driver
cd usb-multidevice-driver
```

---

## Fedora

### Prerequisites

```sh
sudo dnf install gcc make elfutils-libelf-devel dkms \
    kernel-devel-$(uname -r) kernel-devel
```

`kernel-devel-$(uname -r)` gets the headers for the kernel you're running now.
The unversioned `kernel-devel` keeps headers installed for future kernels, so
DKMS can build for them. If dnf can't find the versioned package, your running
kernel is older than the newest one in the repositories. Run
`sudo dnf upgrade`, reboot, and try again.

### Install with DKMS

```sh
make clean                                # don't ship objects from a manual build
sudo mkdir -p /usr/src/mct-t6-0.2.2
sudo cp -r dkms.conf src /usr/src/mct-t6-0.2.2/
sudo dkms install mct-t6/0.2.2
```

With Secure Boot on, Fedora's DKMS signs the module with its own key,
`/var/lib/dkms/mok.pub`. Enroll that key once; see [Secure Boot](#secure-boot).

### Manual build

```sh
make
sudo insmod src/mct_t6.ko
```

### Fedora live USB

A live session keeps installed packages in RAM, so set it up again after every
boot:

```sh
tools/fedora-live-setup.sh
```

This installs the toolchain and the exact `kernel-devel`, builds the module,
runs the host tests, and checks Secure Boot and that the dock is present.
Secure Boot has to be **disabled in the firmware**: a live session has nowhere
to keep an enrolled key. Then run `sudo insmod src/mct_t6.ko`.

---

## Debian / Ubuntu

### Prerequisites

```sh
sudo apt update
sudo apt install build-essential dkms linux-headers-$(uname -r)
```

To keep headers installed for future kernels, also install the matching
meta-package:

| Kernel | Headers meta-package |
|---|---|
| Debian, amd64 | `linux-headers-amd64` |
| Ubuntu, generic | `linux-headers-generic` |
| Ubuntu 22.04 with HWE | `linux-headers-generic-hwe-22.04` |

Ubuntu 22.04's original 5.15 kernel is **too old**. Install the HWE kernel
(`sudo apt install linux-generic-hwe-22.04`) and reboot into it.

### Install with DKMS

```sh
make clean
sudo mkdir -p /usr/src/mct-t6-0.2.2
sudo cp -r dkms.conf src /usr/src/mct-t6-0.2.2/
sudo dkms install mct-t6/0.2.2
```

With Secure Boot on, DKMS signs the module:

- **Ubuntu** uses `/var/lib/shim-signed/mok/MOK.der`. The first DKMS install
  usually asks for a one-time password and enrolls the key at the next boot.
- **Debian** uses `/var/lib/dkms/mok.pub`.

### Manual build

```sh
make
sudo insmod src/mct_t6.ko
```

---

## Arch

### Prerequisites

Install the headers package for **each kernel you use**:

| Kernel | Headers |
|---|---|
| `linux` | `linux-headers` |
| `linux-lts` | `linux-lts-headers` |
| `linux-zen` | `linux-zen-headers` |
| Manjaro `linux61`, `linux612`, … | `linux61-headers`, `linux612-headers`, … |

```sh
sudo pacman -S --needed base-devel dkms linux-headers   # adjust the headers package
```

If `pacman -Syu` has installed a newer kernel than the one running
(`uname -r` doesn't match `pacman -Q linux`), reboot first. The old kernel's
modules directory is gone, so neither `insmod` nor DKMS can build for it.

### Install with DKMS (as a package)

```sh
make clean
cd packaging/arch
makepkg -si
```

This installs `mct-t6-dkms`. The pacman DKMS hook builds the module for every
installed kernel that has headers, now and after each kernel update.

### Manual build

```sh
make
sudo insmod src/mct_t6.ko
```

Arch's stock kernels don't enforce module signatures, even with Secure Boot
set up through `sbctl`, so no signing step is needed unless you have turned on
`module.sig_enforce=1` or lockdown.

---

## Secure Boot

Check whether it's on with `mokutil --sb-state`. If it is, the kernel refuses
unsigned modules: `insmod` fails with `Key was rejected by service`, or
`modprobe` reports `Operation not permitted`.

Pick one way around it:

1. **Enroll the DKMS signing key (recommended for installed systems).**
   ```sh
   sudo mokutil --import /var/lib/dkms/mok.pub              # Fedora, Debian
   sudo mokutil --import /var/lib/shim-signed/mok/MOK.der   # Ubuntu
   ```
   Choose a one-time password and reboot. The blue *MOK manager* screen
   appears: choose *Enroll MOK*, then *Continue*, enter the password, and
   reboot. From then on, DKMS-built modules load.
2. **Disable Secure Boot in the firmware setup.** This is the only option for
   live USBs and quick tests.

A manually built `src/mct_t6.ko` is not signed. Under Secure Boot it loads only
if you sign it with an enrolled key:

```sh
sudo /usr/src/kernels/$(uname -r)/scripts/sign-file sha256 \
    /var/lib/dkms/mok.key /var/lib/dkms/mok.pub src/mct_t6.ko   # Fedora path
```

On Debian, Ubuntu and Arch, `sign-file` is under
`/usr/lib/modules/$(uname -r)/build/scripts/`.

---

## Running and checking it (all distributions)

With DKMS, plug in the dock and the module loads by itself. With a manual
build, run `sudo insmod src/mct_t6.ko` first, before or after plugging in.

```sh
lsusb -d 0711:                              # the dock: 0711:5601 "T6 USB Station"
sudo dmesg | grep mct_t6                    # "Trigger 6: 2 output(s), 58 MiB VRAM, ... high-speed"
grep . /sys/class/drm/card*-{HDMI,VGA}-*/status
```

The dock's monitors then appear in GNOME or KDE display settings like any
other monitor.

Per-output statistics (frames, bytes, JPEG quality, encode and send time,
drops):

```sh
sudo cat /sys/kernel/debug/mct_t6-*/stats
```

### Module parameters

| Parameter | Default | |
|---|---|---|
| `jpeg_quality` | 85 | Maximum JPEG quality, 30–100. Lowered automatically while the USB link is saturated. Changeable at runtime. |
| `encode_jobs` | 0 | Parallel encoder jobs per frame (0 = one per CPU, max 8). Changeable at runtime. |
| `hw_cursor` | true | Use the chip's hardware cursor. Takes effect at load time only. |
| `lossless` | -1 | Resend the settled picture as a lossless raw RGB frame: `1` on any link, `0` never, `-1` on USB 3 links only (a raw 1080p frame is 6.2 MB, too slow for USB 2). Changeable at runtime. |
| `lossless_delay_ms` | 100 | How long the picture must be still before the lossless frame. Changeable at runtime. |
| `lossless_min_fps` | 20 | Send scrolling and video lossless too, while lossless frames still reach this many per second (about 27 for 1080p on USB 3). Below that, motion falls back to JPEG plus a lossless frame once the picture is still. `0` = motion always JPEG. Changeable at runtime. |
| `lossless_always` | false | Send every frame lossless, however slow (still only where `lossless` applies). Rarely needed: `lossless_min_fps` already keeps motion lossless whenever the link allows. Changeable at runtime. |

Change one at runtime:

```sh
echo 92 | sudo tee /sys/module/mct_t6/parameters/jpeg_quality
```

Or set it permanently in `/etc/modprobe.d/mct_t6.conf`:

```
options mct_t6 jpeg_quality=92
```

---

## Updating to a new version of the driver

Your desktop holds the DRM device open while the dock is attached, so the
module can't be unloaded until the dock is unplugged.

**Manual build:**

```sh
make                         # rebuild
# unplug the dock's USB cable
sudo rmmod mct_t6
sudo insmod src/mct_t6.ko
# plug the dock back in
```

**DKMS:**

```sh
sudo dkms remove mct-t6/0.2.2 --all
sudo rm -rf /usr/src/mct-t6-0.2.2
make clean
sudo mkdir -p /usr/src/mct-t6-0.2.2
sudo cp -r dkms.conf src /usr/src/mct-t6-0.2.2/
sudo dkms install mct-t6/0.2.2
```

Then unplug the dock, run `sudo modprobe -r mct_t6`, and plug it back in. On
Arch, run `makepkg -si` again instead.

## Uninstalling

```sh
sudo dkms remove mct-t6/0.2.2 --all && sudo rm -rf /usr/src/mct-t6-0.2.2
# or, on Arch:
sudo pacman -R mct-t6-dkms
```

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Key was rejected by service` | Secure Boot. See [Secure Boot](#secure-boot). |
| `Invalid module format` / `disagrees about version of symbol` | The module was built for a different kernel. Rebuild against the headers for `uname -r`. |
| `make: *** /lib/modules/…/build: No such file or directory` | Headers for the running kernel are missing; see your distro's prerequisites. |
| Module loads, but no `Trigger 6:` line in `dmesg` | Is the dock attached (`lsusb -d 0711:`)? Some docks need their own power supply to start the display chip. |
| `device discovery failed` | The chip answered a setup request unexpectedly. Collect a usbmon capture (see docs/TESTING.md, step 9) and report it. |
| VGA shows `unknown` | Briefly normal for a second after plug-in. If it persists, the monitor's DDC isn't answering: force the output on with the kernel parameter `video=VGA-1:e`. |
| A `corrupt header` EDID dump in `dmesg` at plug-in | Harmless: the VGA port's DDC isn't ready yet, and the driver retries. |
| Laggy or blocky picture | The USB 2.0 link is the limit. The `stats` file shows whether `encode_ms` (CPU) or `send_ms` (USB) dominates. Lower `jpeg_quality` for smoother motion. |

To report a problem, include: `uname -r`, `lsusb -t`, `sudo dmesg | grep -A3 -i mct_t6`,
and the `stats` file.

## Running the tests (developers)

The host tests need Python with Pillow and numpy, plus the sanitizer runtimes:

| Distro | Packages |
|---|---|
| Fedora | `libasan libubsan python3-pillow python3-numpy` |
| Debian / Ubuntu | `python3-pil python3-numpy` (the sanitizers come with `gcc`) |
| Arch | `python-pillow python-numpy` (the sanitizers come with `gcc`) |

```sh
make check
```

See [TESTING.md](TESTING.md) for the compile matrix, the User-Mode Linux
end-to-end test and the hardware checklist.
