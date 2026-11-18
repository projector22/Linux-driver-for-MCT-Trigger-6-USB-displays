# MCT Trigger 6 USB display protocol

This is the protocol `mct_t6` speaks, as far as it is known. Every statement
is backed by at least one of these sources:

| Tag | Source |
|---|---|
| **[MCT]** | MCT's own GPL-2.0 Linux reference driver, [mcttrigger/triggerdm](https://github.com/mcttrigger/triggerdm) (`t6.h`, `t6bulkdef.h`, `t6usbdongle.c`, `main.c`) |
| **[WIN]** | USB captures of MCT's Windows driver driving JCD543 and JUA365 docks, published in [cyrozap/mct-usb-display-adapter-re](https://github.com/cyrozap/mct-usb-display-adapter-re) |
| **[RE]** | cyrozap's protocol notes in the same repository |
| **[PRIOR]** | the experimental [RL-Charles/jcd543-mct-t6-linux-driver](https://github.com/RL-Charles/jcd543-mct-t6-linux-driver) and its test logs |

All multi-byte fields are little-endian. "Head" means one video output; heads are
numbered 0 and 1 by the protocol.

## 1. USB device

The chip enumerates as one vendor-specific device, "T6 USB Station" **[WIN]**.
MCT's Windows bus driver (`t6sta.inf`) binds product IDs `0711:5600`–`561F` and
`0711:5650`–`565F`. It then creates *virtual* child devices for each function,
such as `0711:5621` "Trigger 6 External Graphics" and `0711:5640` audio. Those
IDs never appear on the wire.

| | Value |
|---|---|
| Interface | one, class/subclass/protocol `ff/00/00`, alt setting 0 only |
| `0x81` bulk IN | unused by every known driver |
| `0x02` bulk OUT | all video (and audio) data |
| `0x83` interrupt IN | 64-byte event packets |

**[HW]** j5create HDMI+VGA dock, `0711:5601`, Fedora 6.19 laptop, 2026-10-14:
`bcdUSB 2.00`, `bcdDevice 10.10`, `MaxPower 248mA`, no serial string. The interface
and endpoints are exactly as in the table above (`0x81`/`0x02` bulk, 512-byte
packets; `0x83` interrupt, 64 bytes). The device sat at 480M behind a Genesys
`05e3:0610` hub (typically the USB 2 half of a Genesys USB 3 hub), alongside
the dock's AX88179 USB 3 Ethernet. The laptop port used turned out to be USB 2.0
only (ACPI shows no SuperSpeed peer for it), so the link speed says nothing
about the dock. The hub and the AX88179 report `bcdUSB 2.10` (USB 3 devices
running at USB 2 speed), so the dock is USB 3. The T6 reports `2.00`, so in
this dock it is probably a USB 2 device on the hub's USB 2 side, and may stay at
480M even on a SuperSpeed port. Not yet tried on one.

The chip runs at USB 2.0 High Speed or USB 3.x SuperSpeed, depending on the dock
and the port. On High Speed, uncompressed frames are far too large (1080p60 XRGB
is ~370 MB/s against ~35 MB/s of real bulk throughput), so frames must be JPEG
compressed. MCT's Linux driver uses JPEG whenever the link is slower than
SuperSpeed **[MCT]**.

## 2. Control requests

`bmRequestType` is `0x40` (vendor, OUT, device) or `0xC0` (vendor, IN, device).

| bRequest | Dir | wValue | wIndex | Data | Meaning |
|---|---|---|---|---|---|
| `0x03` | OUT | head | 0 / 1 | – | monitor output off / on **[MCT]** |
| `0x04` / `0x06` | OUT | x | y | – | cursor position, head 0 / 1 **[MCT]** |
| `0x05` / `0x07` | OUT | cursor index | 0 / 1 | – | cursor hide / show, head 0 / 1 **[MCT]** |
| `0x10` / `0x11` | OUT | cursor index | byte offset | ≤ 4096 B | cursor shape upload, head 0 / 1 **[MCT]** |
| `0x12` | OUT | head | 0 | 32 B timing | set display timing **[MCT][WIN]** |
| `0x14` | OUT | 0 | 0 | – | cancel/resync bulk OUT after an error **[MCT]** |
| `0x31` | OUT | head | 0 | – | "software ready", sent after `0x12` **[MCT][WIN]** |
| `0x80` | IN | byte offset | head | 128 B | EDID block (offset = 128 × block) **[MCT][WIN]** |
| `0x84` | IN | head | 0 | u32 | number of entries in the timing table **[MCT]** |
| `0x87` | IN | head | 0 | u8 | connector status, 1 = connected **[MCT][WIN]** |
| `0x88` | IN | 0 | 0 | u8 | video RAM size in MiB (58 on known parts) **[MCT][WIN]** |
| `0x89` | IN | head | byte offset | ≤ 512 B | timing table, 32 B per entry **[MCT][WIN]** |
| `0xB0` | IN | 0 | field | u32 | version info: 0 = hardware platform (0 "Lite", 1 "Super Lite"), 1 = boot code, 2 = image code **[MCT][RE]** |
| `0xB3` | IN | 0 | 0 | 112 B | display section header (below) **[MCT][WIN]** |

Requests the driver deliberately never sends:

- `0x13` and `0x32` write ROM data.
- `0x30` is a hardware reset.
- `0x20`–`0x27` / `0xA0`–`0xA7` are audio.
- `0x1C` / `0x1D` are gamma/LUT control. Windows sends them but MCT's Linux driver does not.

### 2.1 Display section header (`0xB3`, 112 bytes) [MCT]

| Offset | Field |
|---|---|
| `0x0C` | u16 vid of the virtual display function |
| `0x0E` | u16 pid of the virtual display function (`0x5621`) |
| `0x54` | u32 DispFunc: bit 3 "JPEG reset" capability, bits 4–7 rotation |
| `0x58` | u32 head 0 caps |
| `0x5C` | u32 head 1 caps |

Caps word layout:

- Bits 0–3 are LinkInterfaces: bit 0 DAC (VGA), bit 1 DVO (HDMI), bit 2 DVI, bit 3 LVDS (internal HDMI).
- Bits 8–15 hold the number of timing-table entries.
- Bits 16–31 hold the table offset.

A head with LinkInterfaces = 0 does not exist.

### 2.2 Timing record (`0x12`, and `0x89` entries) [MCT][WIN]

| Offset | Size | Field |
|---|---|---|
| `0x00` | u32 | pixel clock, kHz |
| `0x04` | u8 | refresh rate, Hz |
| `0x05` | u8 | reserved |
| `0x06` | u16 | htotal |
| `0x08` | u16 | hdisplay |
| `0x0A` | u16 | hsync start (hdisplay + front porch) |
| `0x0C` | u16 | hsync width |
| `0x0E`–`0x14` | 4 × u16 | vtotal, vdisplay, vsync start, vsync width |
| `0x16` | u16 | PLL fractional numerator P ("FNUM") |
| `0x18` | u16 | PLL fractional denominator Q ("FDEN"), 1000 |
| `0x1A` | u8 | PLL integer N ("IDIV") |
| `0x1B` | u8 | PLL output select / divider |
| `0x1C` | u8 | hsync polarity, 1 = positive |
| `0x1D` | u8 | vsync polarity, 1 = positive |
| `0x1E` | u8 | reduced blanking |
| `0x1F` | u8 | flags (bit 0: custom timing) |

The PLL fields depend on the chip's reference clock (48 MHz "Lite", 40 MHz "Super
Lite") **[MCT]**. MCT's drivers therefore only use timings from the chip's own
table, matched by resolution and refresh rate, and so does `mct_t6`.

Example, 1080p60 as sent by Windows **[WIN]**:

```
14 44 02 00 3c 00 98 08 80 07 d8 07 2c 00 65 04
38 04 3c 04 05 00 bb 02 e8 03 1d 01 01 01 00 00
```

### 2.3 Mode set sequence [MCT][WIN]

1. `0x12` with the head's table entry (wValue = head).
2. `0x31` (wValue = head).
3. `0x03` with wValue = head, wIndex = 1 (output on).

To disable a head, send `0x03` with wIndex = 0.

## 3. Bulk OUT objects

Every write is a 32-byte header, sent as its own transfer, followed by the
payload, which the chip DMAs into its video RAM **[MCT][WIN]**:

| Offset | Field |
|---|---|
| `0x00` | u32 signature: 0 display, 3 audio, 5 ROM flashing, 7 rectangle DMA |
| `0x04` | u32 total payload length |
| `0x08` | u32 destination address in video RAM |
| `0x0C` | u32 length of this fragment |
| `0x10` | u32 fragment offset |
| `0x14` | u8 "more fragments" flag, then 3 reserved bytes |
| `0x18` | 8 bytes function-specific |

MCT's Linux driver sends each object unfragmented: length = fragment length,
offset 0, no flag. Windows fragments into 102400-byte pieces. `mct_t6` sends
unfragmented objects and only ever uses signature 0.

### 3.1 Video header (first 48 bytes of a display payload) [MCT][WIN]

| Offset | Size | Field | JPEG frame value |
|---|---|---|---|
| `0x00` | u32 | command: 3 flip head 0, 4 flip head 1, 7 rectangle | 3 or 4 |
| `0x04` | u32 | payload bytes after this header | JPEG length + 1024 |
| `0x08` | u32 | fence / sequence number, echoed on completion | incrementing |
| `0x0C` | u32 | scan-out format: 6 NV12, 8 XRGB, 9 RGB24 | 6 |
| `0x10` | u16 | Y (or RGB) pitch | `ALIGN(width, 32)` |
| `0x12` | u16 | UV pitch | `ALIGN(width, 32)` |
| `0x14` | u32 | Y plane address | framebuffer slot |
| `0x18` | u32 | UV plane address | slot + `ALIGN(w,32)·ALIGN(h,32)` + 1024 |
| `0x1C` | u32 | V plane address | 0 |
| `0x20` | u32 | source encoding: 6 NV12, 8/9 RGB, 13 JPEG | 13 |
| `0x2F` | u8 | flags: `0x80` reset the JPEG engine | see below |

### 3.2 JPEG streaming [MCT]

- The payload is a 48-byte video header, a complete baseline JFIF file (4:2:0,
  standard Huffman tables are fine), and 1024 zero bytes.
- The object goes to a **ring** address. The chip decodes the JPEG into the NV12
  **framebuffer slot** named in the header and then scans that slot out.
- Ring positions advance in 1, 2 or 3 MiB steps, depending on object size. The
  ring wraps when the next object would cross into the framebuffers.
- The three framebuffer slots are used round-robin.
- Flag `0x80` is set for the first 10 frames after a mode set, and on every ring
  wrap.

Video RAM layout for a 58 MiB part (addresses in MiB) **[MCT]**:

| Case | Ring | Framebuffer slots |
|---|---|---|
| One head | 0 – 46 | 46, 50, 54 |
| Two heads, HDMI-capable head | 0 – 28 | 28, 32, 36 |
| Two heads, other head | 40 – 46 | 46, 50, 54 |

A 4 MiB slot holds NV12 up to about 1920×1200 or 2048×1152, which bounds the
supported modes. 4K needs a different layout and SuperSpeed, which this driver
does not implement.

### 3.3 Raw RGB24 frames [WIN][HW]

The same video header with scan-out and source format 9 (packed B,G,R, pitch
`width * 3`, UV pitch 0, UV address = Y address), immediately followed by the
pixels, written to `framebuffer - 48` so the pixels land on the framebuffer.
Nothing is decoded; the chip scans the surface out directly.

**[HW] 2026-11-09**, j5create dock on a SuperSpeed port, 1920x1080: a whole
6 220 848-byte object sent **unfragmented** (one 32-byte bulk header, then the
payload) is accepted; frame-done is reported for every raw frame (734/734
fences, mixed JPEG and raw, 0 timeouts); ~36 ms per frame, ≈170 MB/s. JPEG and
raw frames can be interleaved freely: each flip switches the scan-out format.
`mct_t6` uses this for lossless refinement (DESIGN.md).

Windows also places JPEGs in a ring and decodes them into triple-buffered NV12
surfaces. It additionally uses "rectangle" objects (command 7) to patch the
currently displayed surface **[WIN]**. `mct_t6` does not use rectangles yet.

## 4. Interrupt IN packets (64 bytes) [MCT][WIN]

| Offset | Field |
|---|---|
| `0x00` | function mask: `0x04` display, `0x20` audio, `0x80` ROM |
| `0x0C` | u32 display data; for frame-done, the completed fence |
| `0x13` | display event bits: `0x01` head 0 hot-plug, `0x02` head 1 hot-plug, `0x04` frame done, `0x08` JPEG decoder error |

MCT's drivers keep this endpoint permanently polled, and re-query `0x87` on any
hot-plug event. The Windows driver sends frames only on damage, with no periodic
refresh **[WIN]**.

## 5. Hardware cursor [MCT]

The shape is an 8-byte header followed by pixels:

- u16 type (1 = ARGB, 0 = mono, 2 = mask colour)
- u16 width
- u16 height
- u16 pitch (width × 4)

Pixels are ARGB8888. The upload is split into control writes of at most 4096
bytes, with wIndex = byte offset into header + pixels. Shapes up to 128×128 are
supported.

Position is set with `0x04` / `0x06`. Negative coordinates are sent as the one's
complement of their magnitude, which is what MCT's driver does.

**[HW]** There are shape slots 0–9 per head (wValue of the shape and state
requests; MCT's `t6.h`). The chip draws the shown slot straight from its
buffer, so uploading into the visible slot (5 writes, about 10 ms for 64x64)
shows half old, half new shape. Showing slot N (`0x05`/`0x07`, wValue N,
wIndex 1) replaces whichever slot was shown, with no hide needed. The driver
therefore double-buffers: upload into the other slot, then show it. Verified on
a j5create dock with KWin, 2026-10-14. MCT's Linux driver only uses slot 0;
the Windows driver never uses the hardware cursor.
