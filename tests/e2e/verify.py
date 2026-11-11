#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Gareth Palmer
"""Check the output of an mct_t6 end-to-end run (see run.sh).

usage: verify.py OUTDIR [--data t6emu_data.h]

OUTDIR holds init.log, dmesg.txt and one directory per pass ("fence",
"nofence", "lossless", "always"), each with t6emu's log.jsonl and frames
(JPEG, or PPM for raw RGB24 frames), t6drm's manifest.json and
expected-NN.rgb images, and the driver's debugfs stats.  "lossless" runs
with lossless=1 (raw refinement after a quiet spell), "always" adds
lossless_always=1 (every frame raw).

Checks, per pass:
  * the emulator reported no protocol validation errors;
  * mode sets are 0x12 -> 0x31 -> 0x03(on), and the first one carries the
    exact 1080p60 record from the chip's timing table;
  * fences strictly increase, and (fence pass) every frame was completed
    on the interrupt endpoint / (no-fence pass) none was;
  * every frame JPEG decodes at 1920x1080;
  * raw frames: none in the JPEG-only passes; in "lossless" every step
    ends on a raw frame and the burst step (two quick flushes) also carries
    a JPEG, so the JPEG-then-refine path runs; in "always" every frame is
    raw; raw frames must equal t6drm's images exactly (they are lossless);
  * the last frame of every t6drm step matches the image t6drm expected,
    and earlier frames of a step match either that or the previous image,
    by PSNR against thresholds calibrated with libjpeg (Pillow) at the
    driver's settings (quality 85, 4:2:0) on the same images;
  * the cursor shape upload (header type 1, 64x64, pitch 256, pixels equal
    to what t6drm drew), then position and show/hide requests, in order;
  * disabling the CRTC turns the output off;
  * the debugfs stats (frames, raw_frames, bytes) agree with what the
    emulator received.
Globally: KUnit (KTAP in dmesg) all ok, init.log has no failures, dmesg has
no warnings, oopses, KASAN or lockdep reports.

Exit status 0 if everything passed, 1 otherwise.
"""
import argparse
import io
import json
import os
import re
import sys

import numpy as np
from PIL import Image

WIDTH, HEIGHT = 1920, 1080
QUALITY = 85            # mct_t6's default jpeg_quality
MARGIN_DB = 2.0         # allowed shortfall against libjpeg at the same settings
FLOOR_DB = 20.0         # sanity floor; the calibrated threshold is the real one

failures = []
warnings = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def warn(msg):
    warnings.append(msg)
    print("WARN: " + msg)


def info(msg):
    print("      " + msg)


# --- Reference data ----------------------------------------------------------

def timing_1080p60(data_h):
    """The head 0 1920x1080@60 record, parsed from t6emu_data.h."""
    text = open(data_h).read()
    head0 = text[text.index("t6_timings_head0"):text.index("t6_timings_head1")]
    m = re.search(r"/\* 1920x1080@60 \*/\s*\{([^}]*)\}", head0)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1)))


def cursor_pixels():
    """The 64x64 ARGB8888 shape t6drm draws, as little-endian bytes."""
    y, x = np.mgrid[0:64, 0:64].astype(np.int64)
    d2 = (x - 32) ** 2 + (y - 32) ** 2
    a = np.where(d2 < 28 * 28, 0xff, np.where(d2 < 31 * 31, 0x80, 0))
    px = (a << 24) | ((x * 4) << 16) | ((y * 4) << 8) | 0x40
    return px.astype("<u4").tobytes()


# --- Images ------------------------------------------------------------------

def load_rgb(path):
    raw = np.fromfile(path, dtype=np.uint8)
    return raw.reshape(HEIGHT, WIDTH, 3)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def crop(img, rect):
    if rect is None:
        return img
    x, y, w, h = rect
    return img[y:y + h, x:x + w]


def libjpeg_roundtrip(img):
    buf = io.BytesIO()
    Image.fromarray(img).save(buf, "JPEG", quality=QUALITY, subsampling=2)
    return np.asarray(Image.open(io.BytesIO(buf.getvalue())).convert("RGB"))


class Thresholds:
    """PSNR a q85 4:2:0 JPEG of an expected image must reach, per region."""

    def __init__(self):
        self.cache = {}

    def get(self, path, img, rect):
        key = (path, tuple(rect) if rect else None)
        if key not in self.cache:
            if path not in self.cache:
                self.cache[path] = libjpeg_roundtrip(img)
            ref = psnr(crop(self.cache[path], rect), crop(img, rect))
            self.cache[key] = (ref, max(FLOOR_DB, ref - MARGIN_DB))
        return self.cache[key]


# --- Per-pass checks ---------------------------------------------------------

def load_log(path):
    events = []
    for n, line in enumerate(open(path), 1):
        try:
            events.append(json.loads(line))
        except ValueError:
            fail("%s:%d: not JSON (truncated log?)" % (path, n))
    return events


def check_protocol(name, events, fences_expected, timing):
    for e in events:
        for err in e.get("errors", []):
            fail("%s: emulator: seq %d %s: %s" %
                 (name, e["seq"], e.get("name", e["ev"]), err))

    ctrl = [e for e in events if e["ev"] == "ctrl"]
    bulk = [e for e in events if e["ev"] == "bulk" and "fence" in e]

    # Mode sets: every 0x12 is followed by 0x31 and 0x03(on) for that head.
    mode_reqs = [e for e in ctrl
                 if e["name"] in ("SET_TIMING", "SOFTWARE_READY",
                                  "MONITOR_CTRL")]
    sets = 0
    for i, e in enumerate(mode_reqs):
        if e["name"] != "SET_TIMING":
            continue
        seq = [x["name"] for x in mode_reqs[i:i + 3]]
        if seq != ["SET_TIMING", "SOFTWARE_READY", "MONITOR_CTRL"] or \
                mode_reqs[i + 2].get("on") != 1 or \
                any(x.get("head") != e["head"] for x in mode_reqs[i:i + 3]):
            fail("%s: mode set at seq %d is %s, not 0x12 -> 0x31 -> 0x03 on"
                 % (name, e["seq"], seq))
        if sets == 0:
            if bytes.fromhex(e["data"]) != timing:
                fail("%s: first mode set sent %s, expected the table's "
                     "1080p60 record %s" % (name, e["data"], timing.hex()))
            else:
                info("%s: mode set 0x12 -> 0x31 -> 0x03 on, 1080p60 record "
                     "matches table entry %d" % (name, e["table_index"]))
        sets += 1
    if not sets:
        fail("%s: no mode set (0x12) seen" % name)
    first_set = next((e["seq"] for e in ctrl if e["name"] == "SET_TIMING"), 0)
    for b in bulk:
        if b["seq"] < first_set:
            fail("%s: video object at seq %d before any mode set" %
                 (name, b["seq"]))

    # Fences.
    fences = [b["fence"] for b in bulk]
    if any(b <= a for a, b in zip(fences, fences[1:])):
        fail("%s: fences not strictly increasing: %s" % (name, fences))
    done = [e["fence"] for e in events if e["ev"] == "int"]
    if fences_expected and done != fences:
        fail("%s: frame-done fences %s != sent %s" % (name, done, fences))
    if not fences_expected and done:
        fail("%s: frame-done packets sent with --no-fence" % name)
    info("%s: %d video objects, fences %s..%s" %
         (name, len(bulk), fences[0] if fences else "-",
          fences[-1] if fences else "-"))
    return bulk


def check_frames(name, d, bulk):
    frames = {}
    for b in bulk:
        if "file" not in b:
            fail("%s: object seq %d has no saved frame" % (name, b["seq"]))
            continue
        try:
            img = Image.open(os.path.join(d, b["file"]))
            img.load()
        except Exception as ex:   # noqa: BLE001 - any decode error fails
            fail("%s: %s does not decode: %s" % (name, b["file"], ex))
            continue
        if img.size != (WIDTH, HEIGHT):
            fail("%s: %s is %dx%d" % (name, b["file"], *img.size))
            continue
        frames[b["frame"]] = b["file"]
    sizes = [b["length"] for b in bulk]
    if sizes:
        info("%s: %d frames decoded; object bytes min %d / mean %d / max %d"
             % (name, len(frames), min(sizes), sum(sizes) // len(sizes),
                max(sizes)))
    return frames


def is_raw(path):
    return path.endswith(".ppm")


def check_steps(name, d, manifest, frames, thr, mode):
    prev_img, prev_file = None, None
    for step in manifest["steps"]:
        if "expected" not in step:
            continue
        exp_file = step["expected"]
        exp = load_rgb(os.path.join(d, exp_file))
        rect = step["rect"]
        rng = list(range(step["frames_before"] + 1, step["frames_after"] + 1))
        label = "%s step %d (%s%s)" % (name, step["step"], step["what"],
                                       " %s" % rect if rect else "")
        if not rng:
            fail("%s: produced no frame" % label)
            continue
        missing = [n for n in rng if n not in frames]
        if missing:
            fail("%s: frames %s missing" % (label, missing))
            continue
        if mode == "lossless" and not is_raw(frames[rng[-1]]):
            fail("%s: final frame %d is JPEG; a lossless refinement was "
                 "due" % (label, rng[-1]))
        if mode == "lossless" and step["what"] == "burst":
            kinds = "".join("R" if is_raw(frames[n]) else "J" for n in rng)
            if "J" not in kinds:
                fail("%s: no JPEG among frames %s (%s): the JPEG-then-"
                     "refine path was not exercised" % (label, rng, kinds))
            else:
                info("%s: frames %s are %s (R raw, J JPEG)" %
                     (label, rng, kinds))
        for n in rng:
            got = np.asarray(Image.open(os.path.join(d, frames[n]))
                             .convert("RGB"))
            last = n == rng[-1]
            if mode == "always" and not is_raw(frames[n]):
                fail("%s: frame %d is JPEG with lossless_always" % (label, n))
            if is_raw(frames[n]):
                # Lossless: exactly the new image, or (not last) the old.
                if np.array_equal(got, exp):
                    info("%s: raw frame %d equals %s exactly" %
                         (label, n, exp_file))
                elif not last and prev_img is not None and \
                        np.array_equal(got, prev_img):
                    info("%s: raw intermediate frame %d equals the previous "
                         "image exactly" % (label, n))
                else:
                    fail("%s: raw frame %d is not pixel-exact (PSNR %.2f dB "
                         "vs %s)" % (label, n, psnr(got, exp), exp_file))
                continue
            ref_g, thr_g = thr.get(exp_file, exp, None)
            p_g = psnr(got, exp)
            ok = p_g >= thr_g
            detail = "PSNR %.2f dB (libjpeg %.2f, need %.2f)" % (p_g, ref_g,
                                                                 thr_g)
            if rect:
                ref_r, thr_r = thr.get(exp_file, exp, rect)
                p_r = psnr(crop(got, rect), crop(exp, rect))
                ok = ok and p_r >= thr_r
                detail += "; rect %.2f dB (libjpeg %.2f, need %.2f)" % (
                    p_r, ref_r, thr_r)
                # The update must be visible: the rect must not still
                # match what was there before.
                if prev_img is not None and last:
                    p_old = psnr(crop(got, rect), crop(prev_img, rect))
                    detail += "; vs previous image %.2f dB" % p_old
                    if p_old >= thr_r:
                        fail("%s: frame %d still matches the previous "
                             "image in the rect (%.2f dB)" % (label, n,
                                                               p_old))
            if last:
                if ok:
                    info("%s: frame %d matches %s: %s" % (label, n, exp_file,
                                                          detail))
                else:
                    fail("%s: final frame %d does not match %s: %s" %
                         (label, n, exp_file, detail))
            elif not ok:
                # An intermediate frame may legitimately show the old image.
                old_ok = prev_img is not None and \
                    psnr(got, prev_img) >= thr.get(prev_file, prev_img,
                                                   None)[1]
                if old_ok:
                    info("%s: intermediate frame %d matches the previous "
                         "image" % (label, n))
                else:
                    fail("%s: intermediate frame %d matches neither image: %s"
                         % (label, n, detail))
            else:
                info("%s: intermediate frame %d matches: %s" % (label, n,
                                                                detail))
        prev_img, prev_file = exp, exp_file


def check_cursor(name, events, manifest):
    ops = [s for s in manifest["steps"] if "cursor" in s]
    if not ops:
        fail("%s: t6drm did not exercise the cursor" % name)
        return
    for op in ops:
        if op["error"]:
            fail("%s: DRM_IOCTL_MODE_CURSOR2 %s failed with errno %d (%s)" %
                 (name, op["cursor"], op["error"],
                  os.strerror(op["error"])))
    if any(op["error"] for op in ops):
        return

    cur = [e for e in events if e["ev"] == "ctrl" and
           e["name"] in ("CURSOR_SHAPE", "CURSOR_POS", "CURSOR_STATE")]
    # Collapse each shape upload (chunks from offset 0) into one item.
    items, shape = [], None
    for e in cur:
        if e["name"] == "CURSOR_SHAPE":
            if e["offset"] == 0:
                shape = {"kind": "shape", "hdr": e, "data": bytearray()}
                items.append(shape)
            if shape is None or e["offset"] != len(shape["data"]):
                fail("%s: cursor chunk at offset %d out of sequence" %
                     (name, e["offset"]))
                return
            if e["wLength"] > 4096:
                fail("%s: cursor chunk of %d bytes" % (name, e["wLength"]))
            shape["data"] += bytes.fromhex(e["data"])
        elif e["name"] == "CURSOR_POS":
            items.append({"kind": "pos", "xy": (e["x"], e["y"])})
            shape = None
        else:
            items.append({"kind": "state", "visible": e["visible"]})
            shape = None

    if not items or items[0]["kind"] != "shape":
        fail("%s: cursor sequence does not start with a shape upload: %s" %
             (name, [i["kind"] for i in items]))
        return
    h = items[0]["hdr"]
    if (h.get("type"), h.get("width"), h.get("height"), h.get("pitch")) != \
            (1, 64, 64, 256):
        fail("%s: cursor header type %s %sx%s pitch %s, expected 1 64x64 256"
             % (name, h.get("type"), h.get("width"), h.get("height"),
                h.get("pitch")))
    data = bytes(items[0]["data"])
    if data[8:] != cursor_pixels():
        fail("%s: uploaded cursor pixels differ from what t6drm drew" % name)
    else:
        info("%s: cursor shape: type 1, 64x64, pitch 256, %d bytes in %d "
             "chunks, pixels match" % (name, len(data),
                                       (len(data) + 4095) // 4096))

    # Expected order: shape, pos, show; pos; pos; hide.  Re-uploads of an
    # unchanged shape are tolerated but reported.
    reuploads = sum(1 for i in items[1:] if i["kind"] == "shape")
    if reuploads:
        warn("%s: the unchanged cursor shape was re-uploaded %d more "
             "time(s) (16 KiB each) while only moving it" % (name, reuploads))
    # The cursor exists so that pointer updates cost a control transfer,
    # not a re-encoded frame: cursor-only commits must not send video.
    last = max((st["frames_after"] for st in manifest["steps"]
                if "expected" in st), default=0)
    dis = [st for st in manifest["steps"] if st.get("disable")]
    if dis:
        resent = dis[0]["frames_before"] - last
        if resent:
            fail("%s: %d cursor-only update(s) re-sent the whole primary "
                 "frame (%d video objects while only the cursor changed)" %
                 (name, len(ops), resent))
        else:
            info("%s: no video traffic during cursor-only updates" % name)
    seq = [(i["kind"], i.get("xy", i.get("visible"))) for i in items
           if i["kind"] != "shape"]
    want = [("pos", (100, 100)), ("state", 1), ("pos", (900, 600)),
            ("pos", (-20, -10)), ("state", 0)]
    if seq != want:
        fail("%s: cursor requests %s, expected %s" % (name, seq, want))
    else:
        info("%s: cursor: position (100,100), show, move (900,600), "
             "move (-20,-10), hide" % name)


def check_disable(name, events, manifest):
    dis = [s for s in manifest["steps"] if s.get("disable")]
    if not dis:
        fail("%s: t6drm did not reach the disable step" % name)
        return
    offs = [e for e in events if e["ev"] == "ctrl" and
            e["name"] == "MONITOR_CTRL" and e.get("on") == 0]
    if not offs:
        fail("%s: disabling the CRTC never sent 0x03 off" % name)
    else:
        info("%s: CRTC disable sent 0x03 off (seq %d)" % (name,
                                                         offs[0]["seq"]))


def check_stats(name, d, bulk, fences_expected, mode):
    path = os.path.join(d, "stats.txt")
    if not os.path.exists(path):
        fail("%s: no debugfs stats captured" % name)
        return
    text = open(path).read()
    for line in text.strip().splitlines():
        info("%s: stats: %s" % (name, line))
    m = re.search(r"head 0: .*?\bframes (\d+) raw_frames (\d+) bytes (\d+)",
                  text)
    if not m:
        fail("%s: stats has no head 0 line" % name)
        return
    frames, raw, nbytes = (int(g) for g in m.groups())
    sent = [b for b in bulk if b.get("head") == 0]
    if frames != len(sent):
        fail("%s: stats count %d frames, emulator received %d" %
             (name, frames, len(sent)))
    raw_sent = sum(1 for b in sent if b.get("raw"))
    if raw != raw_sent:
        fail("%s: stats count %d raw frames, emulator received %d" %
             (name, raw, raw_sent))
    if mode is None and raw_sent:
        fail("%s: %d raw frames with lossless off" % (name, raw_sent))
    if mode is not None and not raw_sent:
        fail("%s: no raw frames with lossless on" % name)
    if mode is not None:
        info("%s: %d of %d frames raw" % (name, raw_sent, len(sent)))
    if nbytes != sum(b["length"] for b in sent):
        fail("%s: stats count %d bytes, emulator received %d" %
             (name, nbytes, sum(b["length"] for b in sent)))
    rep = "(reported)" in text
    if rep != fences_expected:
        fail("%s: stats say fences %s" % (name, "reported" if rep else
                                          "not reported"))
    m = re.search(r"fence_timeouts (\d+)", text)
    if m and int(m.group(1)):
        fail("%s: %s fence timeouts" % (name, m.group(1)))
    m = re.search(r"dropped (\d+)", text)
    if m and int(m.group(1)):
        fail("%s: %s frames dropped" % (name, m.group(1)))


def check_pass(out, name, fences_expected, timing, thr, mode=None):
    d = os.path.join(out, name)
    print("== pass %s" % name)
    log = os.path.join(d, "log.jsonl")
    if not os.path.exists(log):
        fail("%s: no emulator log" % name)
        return
    events = load_log(log)
    bulk = check_protocol(name, events, fences_expected, timing)
    frames = check_frames(name, d, bulk)
    mpath = os.path.join(d, "manifest.json")
    try:
        manifest = json.load(open(mpath))
    except (OSError, ValueError) as ex:
        fail("%s: no usable t6drm manifest (%s)" % (name, ex))
        return
    check_steps(name, d, manifest, frames, thr, mode)
    check_cursor(name, events, manifest)
    check_disable(name, events, manifest)
    check_stats(name, d, bulk, fences_expected, mode)


# --- Global checks -----------------------------------------------------------

DMESG_BAD = [
    r"WARNING:", r"\bBUG\b", r"KASAN", r"Oops", r"general protection",
    r"Kernel panic", r"possible circular locking", r"possible recursive",
    r"inconsistent lock state", r"suspicious RCU", r"lockdep is turned off",
    r"turning off the locking correctness validator",
    r"blocked for more than", r"soft lockup", r"hung_task",
    r"UBSAN", r"refcount_t", r"list_(add|del) corruption", r"ODEBUG",
    r"\*ERROR\*", r"deprecated", r"Call Trace", r"rcu.*stall",
    r"self-detected stall", r"kmemleak",
]


def check_dmesg(out):
    print("== kernel log")
    path = os.path.join(out, "dmesg.txt")
    if not os.path.exists(path):
        fail("no dmesg captured")
        return
    lines = open(path, errors="replace").read().splitlines()
    bad = re.compile("|".join(DMESG_BAD))
    hits = [l for l in lines if bad.search(l)]
    for l in hits[:40]:
        fail("dmesg: " + l.strip())
    if len(hits) > 40:
        fail("dmesg: ... %d more" % (len(hits) - 40))
    if not hits:
        info("no warnings, oopses, KASAN or lockdep reports")

    # KTAP from mct_t6_test.ko.
    cases, suite_ok, totals = [], None, None
    in_suite = False
    for l in lines:
        t = re.sub(r"^\[[^\]]*\] ?", "", l)   # keep KTAP indentation
        if re.match(r"\s*# Subtest: mct_t6\s*$", t):
            in_suite = True
            continue
        if not in_suite:
            continue
        m = re.match(r"\s+(not ok|ok) (\d+) (\S+)", t)
        if m:
            cases.append((m.group(1), m.group(3)))
            continue
        m = re.match(r"\s*# mct_t6: pass:(\d+) fail:(\d+) skip:(\d+) total:(\d+)",
                     t)
        if m:
            totals = tuple(int(x) for x in m.groups())
            continue
        m = re.match(r"(not ok|ok) \d+ mct_t6\b", t)
        if m:
            suite_ok = m.group(1) == "ok"
            break
    if not cases:
        fail("kunit: no mct_t6 test cases in dmesg")
    for res, case in cases:
        if res != "ok":
            fail("kunit: %s %s" % (res, case))
    if suite_ok is not True:
        fail("kunit: suite mct_t6 did not report ok")
    if totals and (totals[1] or totals[0] + totals[2] != totals[3]):
        fail("kunit: totals pass %d fail %d skip %d of %d" % totals)
    if cases and suite_ok:
        info("kunit: mct_t6 ok, %d/%d cases ok (%s)" %
             (sum(r == "ok" for r, _ in cases), len(cases),
              ", ".join(c for _, c in cases)))


def check_init(out):
    print("== guest init")
    path = os.path.join(out, "init.log")
    if not os.path.exists(path):
        fail("no init.log: the guest never ran")
        return
    lines = open(path).read().splitlines()
    for l in lines:
        if l.startswith("FAIL"):
            fail("init: " + l[5:])
    if "done" not in lines:
        fail("init: guest did not finish (hang or crash?)")
    else:
        info("guest completed all steps")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--data", default=os.path.join(here, "t6emu_data.h"))
    args = ap.parse_args()

    timing = timing_1080p60(args.data)
    thr = Thresholds()
    check_init(args.outdir)
    check_dmesg(args.outdir)
    check_pass(args.outdir, "fence", True, timing, thr)
    check_pass(args.outdir, "nofence", False, timing, thr)
    check_pass(args.outdir, "lossless", True, timing, thr, "lossless")
    check_pass(args.outdir, "always", True, timing, thr, "always")

    print("== result: %d failure(s), %d warning(s)" % (len(failures),
                                                       len(warnings)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
