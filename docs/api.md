# KronosScreenRemoteDaemon - API Reference

This document describes every network interface exposed by the `screenremote` daemon: the stream port, the control port, and the UDP discovery probe. All multi-byte integer fields are **little-endian** unless otherwise noted.

---

## Table of contents

1. [Ports and addressing](#1-ports-and-addressing)
2. [UDP discovery](#2-udp-discovery)
3. [Stream port - connection and handshake](#3-stream-port---connection-and-handshake)
4. [Stream port - frame formats](#4-stream-port---frame-formats)
5. [Control port - overview and access control](#5-control-port---overview-and-access-control)
6. [Control port - session modes](#6-control-port---session-modes)
7. [Control port - command reference](#7-control-port---command-reference)
8. [MIDI bridge - port 9875](#8-midi-bridge---port-9875)
9. [Button name reference](#9-button-name-reference)
10. [Analog device code reference](#10-analog-device-code-reference)
11. [SYSINFO field reference](#11-sysinfo-field-reference)
12. [Authentication internals](#12-authentication-internals)
13. [Error handling and disconnection](#13-error-handling-and-disconnection)
14. [Implementation limits](#14-implementation-limits)

---

## 1. Ports and addressing

| Transport | Default port | Configurable | Purpose |
|-----------|-------------|--------------|---------|
| TCP | 7373 | Yes (`stream_port` in config) | Framebuffer stream |
| TCP | 7374 | Yes (`ctrl_port` in config) | Remote control |
| UDP | 7372 | No | LAN discovery |
| TCP | 9875 | No | MIDI bridge; hub mode, up to 8 clients, no auth required; see [Section 8](#8-midi-bridge---port-9875) |

The daemon binds the stream and control ports to the first usable LAN IPv4 address it finds (skipping loopback and link-local). If no LAN interface is up, it waits and retries every 5 seconds. The UDP discovery socket always binds to `INADDR_ANY`.

The daemon accepts only **one stream client at a time**. A new stream connection replaces the existing one.

---

## 2. UDP discovery

Used to locate the daemon on the local network without prior knowledge of its stream and control port numbers.

### Request

Send the 5-byte ASCII string `KSCR?` to UDP port 7372 (broadcast or unicast).

```
Offset  Size  Value
0       5     "KSCR?" (ASCII, no null terminator required)
```

Any payload longer than 5 bytes is accepted as long as the first 5 bytes match.

### Response

The daemon replies to the sender's address and port with a newline-terminated ASCII string.

```
KSCR SP=<stream_port> CP=<ctrl_port> MIDI=<0|1>\n
```

Example:

```
KSCR SP=7373 CP=7374 MIDI=1\n
```

Port numbers are decimal ASCII. `MIDI=1` indicates the MIDI injection module loaded successfully; `MIDI=0` means MIDI functionality is unavailable. There is no authentication on discovery; the daemon always responds.

---

## 3. Stream port - connection and handshake

### 3.1 TCP connection

Connect to the stream port (default 7373). The daemon applies a **5-second receive timeout** for the duration of the handshake. After a successful handshake the timeout is cleared.

The daemon sets `TCP_NODELAY` and enlarges the send buffer to 512 KB (using `SO_SNDBUFFORCE`, which requires the daemon to run as root) to avoid write-stall fragmentation on 480 KB frames.

### 3.2 Client hello

Send immediately after the TCP connection is established.

```
Offset  Size  Field
0       4     Magic: ASCII "KSCR"  (0x4B 0x53 0x43 0x52)
4       1     Protocol version: must be 0x02
5       1     Stream mode (see below)
6       1     Requested FPS (1-15; 0 = use server maximum)
7       1     ulen: username length in bytes (1-64)
8       1     plen: password length in bytes (0-128)
9       ulen  Username (UTF-8, not null-terminated)
9+ulen  plen  Password (UTF-8, not null-terminated)
```

**Stream mode values:**

| Value | Name | Description |
|-------|------|-------------|
| `0x01` | Pull | Client requests each frame explicitly by sending `0xFF` |
| `0x02` | Change | Server pushes frames at the negotiated rate when the display changes |

The server clamps FPS to a maximum of 15. If `fps` is 0 the server uses 15.

### 3.3 Server response - success

```
Offset  Size  Field
0       4     Magic: "KSCR"
4       1     Status: 0x00 (OK)
5       2     Display width in pixels, little-endian (typically 800)
7       2     Display height in pixels, little-endian (typically 600)
9       768   Palette: 256 entries x 3 bytes each (R, G, B), 8 bits per channel
```

Total success response: **777 bytes**.

The palette is the Kronos hardware palette and does not change at runtime. It must be applied by the client to decode 8bpp pixel values to RGB.

### 3.4 Server response - failure

```
Offset  Size  Field
0       4     Magic: "KSCR"
4       1     Status code (see below)
```

**Status codes:**

| Code | Meaning |
|------|---------|
| `0x00` | OK (success path) |
| `0x01` | Authentication failed (bad credentials, or account locked) |
| `0x02` | User not found (no authentication backend recognised the username) |

On any failure the server closes the connection immediately after sending the 5-byte error response.

---

## 4. Stream port - frame formats

After a successful handshake the server begins sending frames. All frames share the same 4-byte little-endian length prefix. The client uses the length value to determine the frame type.

Let `F = width * height` (frame size in bytes, e.g. 480000 for 800x600).

### 4.1 Full frame

Sent when:
- The stream is in Pull mode and the client sends `0xFF`.
- Change mode and no prior frame has been sent yet (first frame after connect).
- Change mode and the PackBits-encoded dirty region would be >= `F` bytes (degenerate case).
- The client sends `REFRESH` on the control port.

```
Offset  Size      Field
0       4         len = F (little-endian)
4       F         Pixel data: raw 8bpp, row-major, left-to-right top-to-bottom
```

Each byte is an index (0-255) into the palette received during the handshake.

### 4.2 Dirty-rect update (Change mode only)

Sent when Change mode detects that only a contiguous band of rows has changed and the compressed payload is smaller than a full frame.

```
Offset  Size      Field
0       4         len = 4 + rle_bytes (little-endian); always < F
4       2         first_row: index of the first changed row (little-endian)
6       2         row_count: number of consecutive changed rows (little-endian)
8       rle_bytes PackBits-compressed pixel data for rows [first_row, first_row+row_count)
```

The client can distinguish a dirty-rect update from a full frame because `len < F`. A full frame always has `len == F`.

**Dirty-rect decoding steps:**

1. Read `first_row` and `row_count`.
2. Decompress `rle_bytes` of PackBits data to obtain `row_count * width` raw bytes.
3. Write those bytes into the client's framebuffer starting at `first_row * width`.
4. Rows outside the updated band are unchanged from the previous frame.

### 4.3 PackBits encoding

The encoding follows standard PackBits (Apple/TIFF variant):

| Header byte range | Meaning |
|-------------------|---------|
| `0x00` - `0x7F` | Literal run: the next `header + 1` bytes are literal pixel values |
| `0x81` - `0xFF` | Repeat run: repeat the next byte `257 - header` times |
| `0x80` | NOP - never emitted by this implementation |

Worst-case expansion is approximately 1/128 overhead (one extra header byte per 128 literal bytes), so the compressed output is at most `n + ceil(n/128)` bytes for input of `n` bytes.

### 4.4 Pull mode frame request

In Pull mode the client controls frame delivery by sending single-byte commands on the stream socket.

```
0xFF  - request one full frame
```

Any other byte, or a closed connection, causes the server to drop the client.

---

## 5. Control port - overview and access control

### 5.1 Purpose

The control port accepts text-line commands (newline-terminated, `\n`) that inject touch events, button presses, wheel ticks, key events, and query daemon state.

### 5.2 Access control

The daemon enforces a strict IP-based access control rule:

- After a stream client successfully authenticates, **only connections from that client's IP address** are accepted on the control port.
- If no stream client is connected, **all control connections are rejected**.
- A control connection from a disallowed IP is immediately closed with no response.

A client must authenticate on the stream port before it can use the control port.

### 5.3 Command format

Commands are plain ASCII, newline-terminated (`\n`). Carriage returns are not stripped but should not be sent. Command names are case-sensitive (uppercase). Arguments are separated by single spaces.

---

## 6. Control port - session modes

### 6.1 One-shot mode (default)

Connect, send one command line, read the response, and the server closes the connection. This is the normal mode for individual commands.

### 6.2 Persistent mode

Send `CTRL_PERSIST\n` as the very first line after connecting. The server keeps the connection open and you may send further commands, one per line, receiving responses inline. A new `CTRL_PERSIST` connection replaces any previously open persistent connection.

In persistent mode the server reads commands with `O_NONBLOCK` and never blocks on the control socket, so slow or stalled clients do not affect stream delivery.

**TOUCH_MOVE coalescing.** Each touch injection is paced ~30ms apart (see `TOUCH`'s section below), so a fast drag gesture can enqueue `TOUCH_MOVE` commands faster than the daemon can pace them through `/proc/.nks4inject`. Processing every buffered move sequentially would delay the trailing `TOUCH_UP` (release) by the full backlog, producing a perceptible lag letting go of a note - live-confirmed 2026-07-14. To keep releases responsive, the persistent-mode reader coalesces `TOUCH_MOVE` into a single pending slot: consecutive `TOUCH_MOVE` lines overwrite the same slot instead of queuing, and the slot flushes (actually injects) as soon as a different command type arrives or the socket's read buffer is fully drained. Only `TOUCH_MOVE` is coalesced this way - every other command (including `TOUCH_UP`) is still processed in full, in order, one per line. A `TOUCH_MOVE` that gets overwritten before it flushes never gets its own `OK\n` reply (the flush that supersedes it runs with no reply socket); the move that does flush - whether because a later command interrupted it or because the buffer drained - gets a normal reply.

---

## 7. Control port - command reference

All commands respond with `OK\n` on success or `ERR\n` on invalid arguments, except where noted.

### 7.0 Front-panel injection architecture (as of 1.10.0)

`TOUCH*`, `BUTTON`, `CHORD`, `WHEEL`, `SLIDER`, `KNOB`, and `VSLIDER` no longer write raw packets to `/dev/rtf5`. Prior versions of this daemon injected events by writing directly to `/dev/rtf5`, the RTAI FIFO Eva reads front-panel events from. That worked well for touch and mode-select buttons but never reliably drove sequencer transport, tempo, or some MIDI-triggering touch actions - because `/dev/rtf5` is actually OA.ko's own **outbound** notification channel to Eva, not an input path. Genuine hardware events reach OA through a separate, internal route (`CSTGOmapNKSMsgHandler::ProcessNextNKSEvent()` pulling raw NKS4 commands and dispatching straight to `CSTGFrontPanel::Handle*`), so a synthetic rtf5 packet only ever fooled Eva's own UI-mirroring logic - it never touched the real OA-side action that channel was originally logging.

As of 1.10.0, these commands write to `/proc/.nks4inject`, exposed by a small companion kernel module (`nks4_inject.ko`, extracted from an embedded buffer and loaded early at startup) that calls OA's real `CSTGFrontPanel::HandleSwitchEvent` / `HandleTouchPanel` / `HandleRotary` / `HandleAnalogController` directly - the exact functions a physical press/touch/turn dispatches through. Injected events get the same response as hardware, independent of whatever mode Eva is currently in.

**If `nks4_inject.ko` failed to load** (symbol resolution failure against `/proc/kallsyms`, the boot-safety kill-switch present, OA not reaching the Live module state, etc.), every command in this section returns `ERR NKS4_NOT_LOADED\n` rather than silently falling back to the old, proven-unreliable `/dev/rtf5` path.

**Input validation policy.** Every numeric argument below is either **snapped** into its valid range or the request is **rejected outright** - never silently passed through out-of-range or forwarded to the kernel unchecked:
- A value with a natural nearest-valid interpretation (a slider/knob index, a 0-127 magnitude, a touch coordinate, a chord hold duration) is **clamped** to the nearest in-range value. `SLIDER 99 500` is accepted as `SLIDER 8 127`, not rejected.
- A value with no sensible "nearest" (an unrecognised button name, a `WHEEL` direction that isn't `CW`/`CCW`, a line that doesn't parse at all) is **rejected** with `ERR\n`.

---

### CTRL_PERSIST

Open a persistent control session.

```
Request:  CTRL_PERSIST\n
Response: (none - connection stays open)
```

Send this as the first and only command in the connection. The server does not reply; subsequent commands on the same connection are processed normally.

---

### MIRROR_ON

Enable the VGA mirror. Creates the flag file `/korg/rw/screenremote/.mirror_enable` and opens `/dev/fb0`. The daemon begins copying fb1 to fb0 on every main loop tick.

```
Request:  MIRROR_ON\n
Response: OK\n
```

---

### MIRROR_OFF

Disable the VGA mirror. Removes the flag file and closes `/dev/fb0` (zeroing it first so fbcon can resume).

```
Request:  MIRROR_OFF\n
Response: OK\n
```

---

### TOUCH

Simulate a complete touchscreen tap (pen-down then pen-up) at pixel coordinates.

```
Request:  TOUCH <x> <y>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n (nks4_inject.ko not loaded)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| x | integer | 0 to width-1 | Horizontal pixel coordinate |
| y | integer | 0 to height-1 | Vertical pixel coordinate |

Coordinates outside the framebuffer are **snapped** to the nearest edge, not rejected. The resulting ADC-space event is delivered via `/proc/.nks4inject` straight into `CSTGFrontPanel::HandleTouchPanel` (see TOUCH_DOWN/TOUCH_UP for the ADC encoding) - the same function a physical finger dispatches through, so any conditional side effect a real touch has (including ones that trigger MIDI output) fires the same way here.

**Why every touch injection is paced ~30ms apart.** A zero-delay pen-down immediately followed by pen-up (or a burst of TOUCH_MOVE steps with no gap) reaches `HandleTouchPanel` only microseconds apart - two back-to-back `write()`s to `/proc/.nks4inject` - versus the tens of milliseconds a real finger's contact/scan naturally spans. Confirmed on hardware: a zero-delay drag across an on-screen knob collapsed into a single tap (the field got selected, but the value never changed) instead of scrubbing the value; the identical sequence paced ~30ms apart worked correctly end to end. This is the same behaviour class as `DAMPER`/`TEMPO` needing a real-time ramp instead of an instant jump (Section 7) - the touch/gesture state machine on the other end needs real elapsed time between samples, not just correct coordinates. The daemon now enforces a minimum ~30ms gap between every touch injection (`TOUCH`'s internal down+up pair, and each `TOUCH_DOWN`/`TOUCH_MOVE`/`TOUCH_UP` a client sends) regardless of how fast the client requests them, so `TOUCH` and multi-step drags both take slightly longer than before but land correctly. This pacing is also the suspected fix for touch-driven "Pads" chord widgets not triggering from injected taps - not yet hardware-confirmed for that specific case.

---

### TOUCH_DOWN

Send a pen-down event only.

```
Request:  TOUCH_DOWN <x> <y>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n
```

Arguments are the same as `TOUCH`. Use `TOUCH_DOWN` followed by zero or more `TOUCH_MOVE` calls and then `TOUCH_UP` to simulate a drag gesture.

---

### TOUCH_MOVE

Send a pen-move (drag) event.

```
Request:  TOUCH_MOVE <x> <y>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n
```

Arguments are the same as `TOUCH`. Should be sent between a `TOUCH_DOWN` and `TOUCH_UP`.

---

### TOUCH_UP

Send a pen-up event.

```
Request:  TOUCH_UP <x> <y>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n
```

Arguments are the same as `TOUCH`.

#### Touch event encoding (internal, `/proc/.nks4inject`)

```
TOUCH <event_type> <coord>\n
  event_type: 1 = pen-down, 2 = pen-up, 3 = pen-move
  coord:      v_adc | (h_adc << 8)
```

ADC values are computed from pixel coordinates using configurable touch calibration parameters (see [Section 14](#14-implementation-limits) for config keys):

```
cx = x + touch_x_offset          (default: 10)
cy = y + touch_y_offset          (default: 20)
h_adc = clamp(round(cx * 255 / touch_x_range), 0, 255)
v_adc = clamp(round(cy * 255 / touch_y_range), 0, 255)
```

With defaults (`touch_x_offset=10`, `touch_x_range=813`, `touch_y_offset=20`, `touch_y_range=638`), pixel (0,0) maps to approximately ADC (3,8) and pixel (799,599) maps to approximately ADC (254,247). The calibration parameters can be adjusted in `screenremote.cfg` if the touch response is misaligned on a particular unit.

`nks4_inject.ko` calls `CSTGFrontPanel::HandleTouchPanel(this, event_type, coord)` with these exact values - `this` resolved from OA's own `CSTGFrontPanel::sInstance`, `event_type`/`coord` passed through unmodified from the values above.

---

### PADCHORD

Play or release one of the 8 "Pads (touch to play)" chords, by pad index rather than by simulating a touch.

```
Request:  PADCHORD <pad_index> <velocity>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n (nks4_inject.ko not loaded)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| pad_index | integer | 0-7 | 0-indexed; pad_index 0 is on-screen "Pad 1", pad_index 7 is "Pad 8" |
| velocity | integer | 0-127 | 0 releases the chord (Note-Off for every voice); 1-127 plays it at that velocity |

Out-of-range values are **clamped**, not rejected, matching this API's usual convention.

**Why this exists instead of just simulating a touch.** Tapping the on-screen Pads grid does *not* go through `CSTGFrontPanel::HandleTouchPanel` the way every other touch-driven widget does - Eva's `CFormOmnimodePads::OnShow()` only arms `CSTGFrontPanel::sInstance`'s on-screen-touch-pad flag when the page is shown with "Enable Pad Play" on, and even then the touch-coordinate math in `HandleTouchPanel` turns out to feed a *different*, single shared KARMA CC trigger - not the per-pad chord table shown on screen. The real per-pad mechanism is `RT_chord_trigger(pad_index, velocity, param3, param4)`, a plain OA.ko function that reads directly from KARMA's own chord-memory tables and calls `Do_KM_note_out_chord_trig()` once per chord voice - confirmed hardware-verified against the on-screen NOTE/VEL grid for all 8 pads. `PADCHORD` calls it directly, bypassing touch/Eva entirely. `param3` is fixed at `1`, which enables `ScaleByte()` proportional velocity rescaling (the incoming velocity becomes the chord's new "max" reference note, with the other voices scaled to preserve their relative balance) - confirmed correct on hardware. `param4` is fixed at `1` - not yet shown to need to vary.

**Address resolution.** `RT_chord_trigger` has no standalone `/proc/kallsyms` entry (internal linkage, unlike the four `CSTGFrontPanel::Handle*` methods above). Its live address is derived from a fixed file-offset delta against `Do_KM_note_out_chord_trig`, which *is* kallsyms-visible - both live in the same `OA.ko`, so the delta survives relocation. If `Do_KM_note_out_chord_trig` is ever renamed or the delta goes stale (OS update changing `OA.ko`'s layout), `PADCHORD` fails gracefully with `ERR NKS4_NOT_LOADED`-style unavailability (screenremote logs `fn_chord not resolved` at startup) while every other command keeps working - re-derive both addresses with `nm` against the current `OA.ko` if that happens (see `resolve_nks4_kallsyms()`'s own comment in `screenremote.c`).

---

### PADMAP / PADMAP_LIST / PADMAP_ON / PADMAP_OFF / LASTTOUCH

Bridges real taps on the on-screen "Pads (touch to play)" grid straight to `PADCHORD`, so a client doesn't need to know about `PADCHORD` at all - tapping the grid through any existing touch client (`TOUCH`, `TOUCH_DOWN`/`TOUCH_UP`) just plays the right chord. Detection stays off by default (`PADMAP_OFF`) even though the regions below are calibrated - flip `PADMAP_ON` once ready.

**Region calibration (2026-07-14).** All 8 pads sit in a single row spanning the full width of the screen, calibrated from 32 real corner taps (4 per pad, captured live via `LASTTOUCH` through `tools/pad_calibration_monitor.py`) then quantized to uniform 94x335 boxes by linear regression on the pad centers (raw click widths varied 88-97px purely from click imprecision - centers fit a clean line, ~99.8px spacing, ~6px gaps):

| pad_index | x0 | y0 | x1 | y1 |
|---|---|---|---|---|
| 0 | 3 | 144 | 96 | 478 |
| 1 | 103 | 144 | 196 | 478 |
| 2 | 203 | 144 | 296 | 478 |
| 3 | 303 | 144 | 396 | 478 |
| 4 | 402 | 144 | 495 | 478 |
| 5 | 502 | 144 | 595 | 478 |
| 6 | 602 | 144 | 695 | 478 |
| 7 | 702 | 144 | 795 | 478 |

```
Request:  PADMAP <pad_index> <x0> <y0> <x1> <y1>\n
Response: OK\n
          ERR\n   (line doesn't parse as 5 integers, or pad_index out of 0-7)
```
Live-sets `pad_index`'s rectangular hit box (inclusive, framebuffer pixel space) in memory - no daemon restart needed.

```
Request:  PADMAP_LIST\n
Response: <idx> <x0> <y0> <x1> <y1>\n   (one line per pad, 8 lines)
```

```
Request:  PADMAP_ON\n / PADMAP_OFF\n
Response: OK\n
```
Toggles auto-detection. `PADMAP_OFF` also releases whatever pad is currently held (if any).

```
Request:  LASTTOUCH\n
Response: X=<x> Y=<y>\n
```
Returns the most recent touch's raw framebuffer pixel coordinates (updated by every `TOUCH`/`TOUCH_DOWN`/`TOUCH_MOVE`/`TOUCH_UP`, regardless of `PADMAP_ON`/`OFF`) - the calibration primitive: tap a real on-screen pad through any client, then read back where that tap actually landed to narrow down its true box with `PADMAP`.

---

### PADMAP_STATE

Diagnostic dump of the internal state `pad_hit_test()`/`inject_touch()` actually use, for debugging why `PADMAP_ON` isn't firing `PADCHORD`. See "How detection works" and "Debugging a tap that doesn't fire" below for how to read the fields.

```
Request:  PADMAP_STATE\n
Response: ENABLED=<0|1> ACTIVE_PAD=<n> NKS4_LOADED=<0|1> PAD_PLAY_ACTIVE=<0|1> ON_PADS_PAGE=<0|1>
          ENABLE_PAD_PLAY=<0|1> CHORD_ASSIGN=<0|1> FIXED_VELOCITY=<0|1>
          LAST_TOUCH_TYPE=<0-3> LAST_GATE_PADS_PAGE=<0|1> LAST_GATE_PAD_PLAY=<0|1>
          LAST_GATE_CHORD_ASSIGN=<0|1> LAST_GATE_HIT=<0|1>\n
```

| Field | Description |
|-------|-------------|
| `ENABLED` | Current `PADMAP_ON`/`PADMAP_OFF` state |
| `ACTIVE_PAD` | Index of the pad currently held via detected-tap bridging, or -1 if none |
| `NKS4_LOADED` | 1 if `nks4_inject.ko` loaded successfully |
| `PAD_PLAY_ACTIVE` | `pad_play_active()` - retained for reference; does **not** gate firing (see prose above) |
| `ON_PADS_PAGE` | Live result of the `on_pads_page()` framebuffer pixel fingerprint |
| `ENABLE_PAD_PLAY` | Live state of the "Enable Pad Play" on-screen toggle |
| `CHORD_ASSIGN` | Live state of the "Chord Assign" on-screen toggle |
| `FIXED_VELOCITY` | Live state of the "Fixed Velocity" on-screen toggle |
| `LAST_TOUCH_TYPE` | Most recent touch event type: 1=down, 2=up, 3=move |
| `LAST_GATE_PADS_PAGE` | `on_pads_page()` result at the most recent pen-down gate evaluation |
| `LAST_GATE_PAD_PLAY` | `enable_pad_play_on()` result at the most recent pen-down gate evaluation |
| `LAST_GATE_CHORD_ASSIGN` | `chord_assign_on()` result at the most recent pen-down gate evaluation |
| `LAST_GATE_HIT` | Whether `(x, y)` fell inside a configured pad box at the most recent pen-down |

Each `LAST_GATE_*` field is captured independently rather than short-circuited, so a failed tap can be diagnosed by seeing exactly which condition blocked it.

---

### PALETTE

Dump the full 256-entry RGB palette (the same table sent to stream clients at handshake) as a single hex string - a calibration aid for translating a raw `fb1` palette index (from `PIXEL`/`REGION`) into an actual color, e.g. to confirm a toggle indicator is really "red" and find its on/off palette indices.

```
Request:  PALETTE\n
Response: <768 hex chars>\n   (256 entries x 3 bytes RGB, no separators)
```

Byte `3*i`..`3*i+2` of the decoded response is the `(R, G, B)` triple for palette index `i` - the same layout and values as the 768-byte palette block in the stream handshake (Section 3.3).

---

### REGION

Hex-dump a rectangle of raw `fb1` palette-index bytes in one round trip - for diffing a before/after snapshot around a UI change (e.g. a toggle) to find exactly which pixels changed, since per-pixel `PIXEL` queries are too slow for a wide-area scan.

```
Request:  REGION <x0> <y0> <x1> <y1>\n
Response: W=<w> H=<h> <hex bytes>\n
          ERR\n   (line doesn't parse as 4 integers, or the rectangle exceeds 8192 pixels)
```

| Argument | Type | Description |
|----------|------|-------------|
| x0, y0 | integer | One corner of the rectangle, **clamped** into the framebuffer bounds |
| x1, y1 | integer | The opposite corner, **clamped** into the framebuffer bounds; corners are normalised (swapped if given in the wrong order) |

`w = x1 - x0 + 1`, `h = y1 - y0 + 1` after clamping/normalising. The response body is `w * h` hex-encoded bytes (2 hex chars per pixel), row-major starting at `(x0, y0)`, each byte a raw palette index into the table returned by `PALETTE`. Capped at 8192 pixels total (see [Section 14](#14-implementation-limits)) to keep the response buffer bounded; a request exceeding that returns `ERR\n` rather than truncating.

---

### PIXEL

Read the raw `fb1` palette-index byte at a single pixel - a calibration aid for finding a page-identifying pixel (e.g. something unique to one Kronos screen) the same way `PADMAP`'s regions and the on-screen toggle indicators were calibrated.

```
Request:  PIXEL <x> <y>\n
Response: V=<n>\n
          ERR\n   (line doesn't parse as two integers, or (x, y) is outside the framebuffer)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| x | integer | 0 to width-1 | Horizontal pixel coordinate; out-of-range is **rejected**, not clamped |
| y | integer | 0 to height-1 | Vertical pixel coordinate; out-of-range is **rejected**, not clamped |

`n` is the same raw palette index space as `REGION` and the stream's pixel data - look it up in `PALETTE`'s response to get an actual RGB color.

---

**How detection works.** On pen-down (`TOUCH`'s implicit down, or `TOUCH_DOWN`), if `PADMAP_ON`, `(x, y)` falls inside a configured pad's box, AND `pad_play_active()` is true (see below), that pad's index is remembered and `PADCHORD <pad> <velocity>` fires immediately. On the matching pen-up, `PADCHORD <pad> 0` releases it unconditionally (not re-gated - if a chord was started, it always gets cleanly released even if the page/mode changed mid-hold). The underlying `TOUCH` event is still forwarded to Eva as normal either way - this is additive, not a replacement, since Eva's own touch handling is otherwise unaffected by any of this.

**Page/mode gating.** The Pads page has real modal state that changes what a tap even means: "Enable Pad Play" must be on for taps to produce sound at all, and while "Chord Assign" is active a tap instead *assigns* whatever chord is currently held on the keybed to that pad rather than playing it - firing `PADCHORD` in either wrong state would be surprising or actively disruptive.

The first approach tried - reading `onscreen_touch_pad_mode` from `/proc/.nks4inject_status` (`CSTGFrontPanel::sInstance[0x104]`, already exposed by `nks4_inject.c`) - looked promising statically (`CFormOmnimodePads::OnHide()` unconditionally clears it on navigating away, `OnShow()` only sets it with Enable Pad Play on) but was **live-tested and disproved** (2026-07-14): the flag read 0 while a physical tap was actively producing sound. It most likely gates the separate, already-confirmed-unrelated single-shared-KARMA-CC touch path (see `PADCHORD`'s "why this exists" note above), not per-pad triggering or page state. `pad_play_active()` and `PADMAP_STATE`'s `PAD_PLAY_ACTIVE` field still exist for reference but do **not** gate firing.

What actually gates firing now is `on_pads_page()`, a **framebuffer pixel fingerprint**: a point 5px inset from each of the 8 pad boxes' top-left corners (`fb1_map`, a direct `mmap()` onto the hardware framebuffer, so always live with zero staleness) all read the same uniform pad-box-background palette value (`219`) while confirmed on the Pads page, and none of the 8 read that value on a different tab (live-tested, mostly `15` with a couple outliers) - requiring all 8 to match is a strong discriminator, since another page coincidentally matching all 8 specific coordinates is very unlikely. `PADMAP_STATE`'s `ON_PADS_PAGE` field reports this live.

**Enable Pad Play / Chord Assign / Fixed Velocity - calibrated (2026-07-14).** All three have a dedicated on-screen LED-style toggle indicator: a small red dot, bright red (palette index ~50, R≈246) when ON, dark/muted red (index ~59-61, R≈105-129) when OFF - found by capturing a `REGION` snapshot before and after the user toggled each one live and diffing to find exactly which pixels changed (the click coordinate itself is offset from the actual indicator - `PIXEL` sampling at the click point alone showed no change). `toggle_is_on(x, y)` checks the palette's R channel against a threshold (200) rather than requiring an exact index match, for tolerance to minor rendering variance. Calibrated pixel coordinates, remarkably evenly spaced (340px apart, likely one shared widget template repeated along the row):

| Toggle | Pixel (x, y) |
|---|---|
| Chord Assign | (59, 102) |
| Enable Pad Play | (399, 103) |
| Fixed Velocity | (739, 102) |

`PADCHORD` now only fires when `on_pads_page() && enable_pad_play_on() && !chord_assign_on()`. `PADMAP_STATE` reports all three (`ENABLE_PAD_PLAY`, `CHORD_ASSIGN`, `FIXED_VELOCITY`).

**Fixed Velocity - resolved (2026-07-14).** Confirmed by physically tapping the real Kronos with Fixed Velocity on: the hardware fires every chord at velocity 127 regardless of tap force or position, ignoring the Y-based curve entirely - not a separately-configured number as originally assumed. `pad_hit_test()` now sends 127 whenever `fixed_velocity_on()` is true, bypassing `velocity_for_touch_y()` for that tap.

**Fragility note.** All of this (pad regions, page fingerprint, toggle indicators) is calibrated against exact pixel coordinates and colors on the current Kronos OS version's rendering of this page. A Korg firmware update that changes this page's layout, colors, or widget positions would silently break some or all of it - there's no way to detect that from here short of noticing PADCHORD stops firing (or fires on the wrong page/pad). Re-run the same before/after `REGION`-diff calibration process if that ever happens.

**Minimum hold duration.** A near-instantaneous tap (pen-down immediately followed by pen-up - e.g. `TOUCH`'s own single-shot down+up pair with no delay) passed every gate condition (confirmed via `PADMAP_STATE`'s `LAST_GATE_*` fields) yet produced no MIDI at all, live-tested 2026-07-14, while a manually-paced test (200ms between down and up) worked correctly - `RT_chord_trigger`'s Note-On apparently needs more processing time than a zero-delay down+up pair gives it before the Note-Off cancels it. `inject_touch()`'s pen-up handler now enforces `PADCHORD_MIN_HOLD_MS` (80ms) between trigger and release, blocking the daemon's main loop briefly if needed (same accepted pattern as `touch_pace()`'s existing `TOUCH_MIN_INTERVAL_MS` sleep) - acceptable for a rare, user-initiated tap.

**Debugging a tap that doesn't fire.** `PADMAP_STATE`'s `LAST_TOUCH_TYPE` (1=down, 2=up, 3=move) and `LAST_GATE_*` fields (`PADS_PAGE`, `PAD_PLAY`, `CHORD_ASSIGN`, `HIT`) record the most recent pen-down's gate evaluation, each condition captured separately rather than short-circuited - read it right after a failed tap to see exactly which condition blocked it (or confirm they all passed, pointing at a downstream/timing issue instead).

**Velocity curve - calibrated against real hardware (2026-07-14).** `velocity = clamp(round(-0.35148 * touch_y + 163.771), 1, 127)` - a **global** linear function of absolute framebuffer Y, independent of which pad or that pad's own box bounds (not relative to each pad's own top/bottom as an earlier version guessed). Derived from 10 paired (touch_y, resulting MIDI velocity) samples captured live with `tools/pad_calibration_monitor.py`: a 7-pad diagonal sweep plus 4 repeated taps on one single pad at different heights (same X, to isolate Y from pad identity) - both datasets agree, R²=0.99. Method: click through a client *and* physically touch the same on-screen spot within ~1s (client-only clicks don't produce MIDI - confirming that gap is exactly why this bridge exists), and watch `LASTTOUCH` alongside the live MIDI-out feed. Pad hit-box boundaries (the `PADMAP` X/Y ranges themselves) are still uncalibrated placeholders - the diagonal sweep hints pads are ordered left-to-right by X (spanning most/all of the screen height each), but a horizontal sweep at fixed Y is needed to pin down real X boundaries.

---

### BUTTON

Press and release a named front-panel button. See [Section 9](#9-button-name-reference) for the full list of button names.

```
Request:  BUTTON <name>\n
Response: OK\n                  (button found)
          ERR\n                 (button name not recognised - no valid "nearest" to snap to)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Description |
|----------|------|-------------|
| name | string | Button name from the button table (case-sensitive, uppercase) |

The event is delivered as two commands on `/proc/.nks4inject` - `BTN <code>` implies a press then release internally (see below). This calls `CSTGFrontPanel::HandleSwitchEvent(this, code, pressed)` directly, the same function a physical press dispatches through, so the full real action fires (including RT-domain effects rtf5 injection never reached - sequencer transport, KARMA control-surface state changes, etc.). Mode-select buttons (SETLIST, COMBI, etc.) also update the daemon's internal mode state so `STATE` queries reflect the change immediately.

#### Button event encoding (internal, `/proc/.nks4inject`)

```
BTN <code>\n        press + release
BTN_DOWN <code>\n   press only (used by CHORD)
BTN_UP <code>\n     release only (used by CHORD)
```

`code` is the button's flat NKS4 hardware scan code (0-127) - see [Section 9](#9-button-name-reference). This is a single value, not the `(dev, code)` pair earlier versions of this document described; that pairing was specific to the old `/dev/rtf5` packet format and does not apply here.

---

### CHORD

Press two or more buttons as a chord: buttons are pressed left-to-right, then released right-to-left, so all buttons are held simultaneously at the midpoint.

```
Request:  CHORD [<hold_ms>] <name1> <name2> [<name3> ... <name8>]\n
Response: OK\n                  (all buttons found)
          ERR\n                 (fewer than 2 names, or any button name not recognised)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Description |
|----------|------|-------------|
| hold_ms | integer | Optional hold duration in milliseconds, **snapped** into [0, 5000] (a negative value snaps to 0, anything above 5000 snaps to 5000). If omitted, buttons are released immediately after all are pressed. |
| name1 | string | First button (held down first, released last) |
| name2 | string | Second button |
| name3..name8 | string | Optional additional buttons (up to 8 total) |

For N buttons, 2xN `/proc/.nks4inject` commands are sent: N `BTN_DOWN` in order, then N `BTN_UP` in reverse order. If `hold_ms` is specified, the daemon sleeps that duration between the press and release phases (blocks the main loop for the duration). Unlike button *names* (which have no sensible fallback and are rejected), `hold_ms` is a plain magnitude and is snapped rather than rejected.

---

### WHEEL

Send one data wheel tick.

```
Request:  WHEEL CW\n
          WHEEL CCW\n
Response: OK\n
          ERR\n                 (direction not CW or CCW - no numeric value to snap)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Value | Description |
|----------|-------|-------------|
| CW | clockwise | Increment (positive direction) |
| CCW | counter-clockwise | Decrement (negative direction) |

#### Wheel event encoding (internal, `/proc/.nks4inject`)

```
ROT <delta>\n
  delta: 256 (0x00000100) for CW, 65280 (0x0000FF00) for CCW
```

Calls `CSTGFrontPanel::HandleRotary(this, delta)` directly. These exact 32-bit values are ground-truthed from a real hardware capture - `HandleRotary`'s delta argument is zero-extended from the raw 16-bit NKS4 field, **not** a signed `-256` for CCW; sending a signed negative value here would not reproduce real hardware behaviour.

---

### SLIDER

Set the position of physical Slider n (1-8) - the linear faders, e.g. the ones used for track/timbre volume.

```
Request:  SLIDER <n> <value>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| n | integer | 1-8 | Slider index (1 = leftmost), **snapped** into range |
| value | integer | 0-127 | Absolute position, **snapped** into range |

The physical effect (which parameter each slider controls) depends on the active Control Assign page on the Kronos - this is genuine hardware behaviour identical to moving the physical slider, not a quirk of injection.

#### Encoding (internal, `/proc/.nks4inject`)

```
ANALOG <device_code> <byte0> <byte1>\n
  device_code: 16 + (n - 1)     (physical sliders occupy device codes 16-23)
  byte0:       value * 2
  byte1:       0
```

Calls `ShortInvertNkS4AnalogValue(byte0, byte1, &out_hi, &out_lo)` (OA's own raw-ADC transform) followed by `CSTGFrontPanel::HandleAnalogController(this, device_code, out_hi, out_lo)`. With `byte1=0`, the transform simplifies to `out_hi = byte0 >> 1`, which is what the consuming Slider handler reads as the 0-127 display value - hence `byte0 = value * 2` reproduces `value` exactly. Confirmed empirically on hardware: a `byte0` sweep from 0 to 224 in steps of 32 moved the on-screen value in a clean, exact staircase (steps of 16).

---

### KNOB

Set the position of physical RT Knob n (1-8) - the rotary Realtime Controls / KARMA knob row, addressed separately from the sliders above.

```
Request:  KNOB <n> <value>\n
Response: OK\n
          ERR\n                 (line does not parse as two integers)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| n | integer | 1-8 | Knob index (1 = leftmost), **snapped** into range |
| value | integer | 0-127 | Absolute position, **snapped** into range |

#### Encoding (internal, `/proc/.nks4inject`)

Identical to `SLIDER` above except `device_code = 8 + (n - 1)` (RT knobs occupy device codes 8-15, a completely separate range from sliders - they are two distinct physical control rows on the Kronos, not the same controls in a different mode).

---

### VSLIDER

Set the value slider position - the single slider that edits whatever field is currently highlighted/selected on screen. On real hardware this control does nothing when nothing is selected; the same is true here.

```
Request:  VSLIDER <value>\n
Response: OK\n
          ERR\n                 (line does not parse as an integer)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| value | integer | 0-127 | Absolute slider position, **snapped** into range |

The value slider changes the currently selected on-screen parameter proportionally. Moving to 0 sets the parameter minimum; 127 sets the maximum. If nothing is currently highlighted for value-slider editing, the call still succeeds (`OK\n`) but has no visible effect - matching real hardware.

#### Encoding (internal, `/proc/.nks4inject`)

Identical to `SLIDER`/`KNOB` above with `device_code = 25` (a single fixed value slider control, not an 8-wide indexed group). **Device code 24 is a different, context-dependent effects-rack parameter edit control, not the value slider** - an easy mistake to make since it sits directly adjacent; it is not exposed by any command in this daemon.

---

### JOYSTICK, VECTOR, RIBBON, AFTERTOUCH, PEDAL, FOOTSWITCH, DAMPER

All seven of these commands are **hardware-confirmed** against a real unit (`RIBBON`'s `Z` axis is the one remaining untested exception - its device code is real, but no test has verified what its value means).

```
Request:  JOYSTICK <X|Y> <value>\n
          VECTOR <X|Y> <value>\n
          RIBBON <X|Z> <value>\n
          AFTERTOUCH <value>\n
          PEDAL <value>\n
          FOOTSWITCH <value>\n
          DAMPER <value>\n
Response: OK\n
          ERR\n                 (bad axis letter, or line does not parse)
          ERR NKS4_NOT_LOADED\n
```

| Command | Argument | Range | device_code | Description |
|---------|----------|-------|-------------|-------------|
| `JOYSTICK` | axis `X` or `Y` | - | 1 (X) / 2 (Y) | Standard Joystick (Kronos manual item 12) - pitch bend / vibrato-wah. Confirmed via a full-radius clockwise circular sweep then a half-radius counter-clockwise sweep. Axis letter is rejected outright if not `X`/`Y` (no numeric fallback to snap to); `value` is snapped into [0,127] |
| `VECTOR` | axis `X` or `Y` | - | 5 (X) / 6 (Y) | **Vector Joystick** (item 9) - a separate physical control from `JOYSTICK`, used for Vector Synthesis blending, not pitch/mod. Confirmed the same way as `JOYSTICK` |
| `RIBBON` | axis `X` or `Z` | - | 3 (X) / 4 (Z) | Ribbon controller (item 13) touch strip. `X` (finger position) confirmed via a center/max/center/min/center sweep. `Z` axis meaning (commonly touch pressure) is real but **not** hardware-verified |
| `AFTERTOUCH` | value | 0-127 | 7 | Keybed channel aftertouch. Confirmed via a 0/half/full/half/0 sweep |
| `PEDAL` | value | 0-127 | 27 | Rear-panel assignable PEDAL jack. Confirmed via a 0/half/full/half/0 sweep - large single-step jumps work fine |
| `FOOTSWITCH` | value | 0-127 | 28 | Rear-panel assignable foot SWITCH jack. Confirmed via a single on(127)/off(0) tap, including that polarity (normally-open/closed) doesn't affect this injection path - it's resolved at the physical ADC layer, before the byte reaches this pipeline. Any value in range is accepted and forwarded rather than restricted to 0/127 |
| `DAMPER` | value | 0-127 | 29 | Rear-panel DAMPER jack - sustain, or half-damper position if the current Program/Combi has half-damper response configured. Ramped internally (see below) - send a target value like any other command here |

All seven `value` arguments are **snapped** into [0,127], matching `SLIDER`/`KNOB`/`VSLIDER`. `JOYSTICK`/`VECTOR`/`RIBBON`'s axis letter has no numeric "nearest" to snap to, so an unrecognised axis is **rejected** with `ERR\n`, the same policy as `WHEEL`'s direction argument.

**Why `DAMPER` ramps internally.** The jack accepts either a simple on/off footswitch (Korg PS-1) or a continuous half-damper pedal (Korg DS-1H), and `AnalogDamperHandler` evidently uses rate-of-change to tell them apart - this is *not* a polarity-setting effect (a polarity change was tried first, based on the odd initial symptoms, and made no difference). A direct jump to a target value produced inconsistent, non-repeatable results on real hardware across two otherwise-identical test runs; a gradual ramp through every intermediate value did not - confirmed via a 256-step sweep (1 unit per step, ~40ms/step, full `0`-`127`-`0` sweep), smooth and repeatable in both directions. `DAMPER` therefore steps to its target internally rather than jumping - see [Section 7.0](#70-front-panel-injection-architecture-as-of-1100)'s `nks4_analog_ramp()` reference and `screenremote.c`'s own comment for the implementation. This blocks the daemon's main loop for the ramp's duration, the same tradeoff `CHORD`'s `hold_ms` already makes elsewhere in this API.

#### Encoding (internal, `/proc/.nks4inject`)

Identical to `SLIDER`/`KNOB`/`VSLIDER`: `ANALOG <device_code> <value*2> 0`. See [Section 10](#10-analog-device-code-reference) for the full device code table and confidence level of each.

---

### TEMPO

Set the song/pattern tempo. **Hardware-confirmed**, including a full sweep verified identical in both directions.

```
Request:  TEMPO <value>\n
Response: OK\n
          ERR\n                 (line does not parse as an integer)
          ERR NKS4_NOT_LOADED\n
```

| Argument | Type | Range | Description |
|----------|------|-------|--------------|
| value | integer | 0-127 | **Not** a direct BPM number - snapped into [0,127] like every other analog command, then mapped onto tempo through the confirmed curve below |

`TEMPO` is device code 26, and - like `DAMPER` - ramps internally rather than jumping directly to the target, for the same reason: a direct jump to a target value produces inconsistent, non-repeatable results on real hardware, while a smooth monotonic ramp through every intermediate value is precisely reproducible. This was confirmed with a full `0`-`127` sweep in both directions, landing on **identical BPM values ascending and descending**:

| value | 0 | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 127 |
|-------|---|----|----|----|----|----|----|----|-----|
| bpm | 40.00 | 51.00 | 68.00 | 92.00 | 120.00 | 154.00 | 196.00 | 245.00 | 297.00 |

`value=0` lands almost exactly on the documented minimum (40bpm) and `value=127` almost exactly on the documented maximum (300bpm - measured 297). The curve is **not linear** - the BPM gap between consecutive sample points grows steadily (11, 17, 24, 28, 34, 42, 49, 52), meaning `TEMPO` has more resolution at low tempos than high ones. No closed-form formula is exposed here; interpolate between these nine confirmed points if a client needs a specific intermediate BPM, since the measured data is the more reliable source than a guessed curve fit.

#### Encoding (internal, `/proc/.nks4inject`)

Same `ANALOG <device_code> <value*2> 0` encoding as every other analog command, with `device_code = 26`, but sent as a ramp (one `ANALOG` call per intermediate value, ~25ms apart) from the last value this daemon commanded rather than a single jump - see `nks4_analog_ramp()` in `screenremote.c`. The first `TEMPO` call after daemon startup has no known starting point to ramp from and jumps directly as a best-effort default; every call after that ramps from the previous `TEMPO` call's target. A physical hand on the front-panel tempo control between `TEMPO` calls would desync this tracking, the same caveat any absolute-position software control over a real analog input has.

---

### KEY

Inject a raw Linux key event by keycode.

```
Request:  KEY <code> <val>\n
Response: OK\n
          ERR\n (out-of-range or invalid arguments)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| code | integer | 1-511 | Linux input keycode (`KEY_*` constants from `<linux/input.h>`) |
| val | integer | 0 or 1 | 0 = key release, 1 = key press |

Events are delivered to `/proc/.vkbd` (vkbd.ko virtual keyboard) if available, otherwise via a `/dev/uinput` virtual device.

---

### REFRESH

Force the Change-mode stream to send a full frame on the next tick, regardless of whether the framebuffer has changed.

```
Request:  REFRESH\n
Response: OK\n
```

Has no effect in Pull mode (the client already controls frame requests with `0xFF`).

---

### SS_TIMEOUT

Change the VGA screensaver timeout at runtime without restarting the daemon.

```
Request:  SS_TIMEOUT <n>\n
Response: OK\n
          ERR\n (not a non-negative integer)
```

| Argument | Type | Description |
|----------|------|-------------|
| n | integer | Seconds of no fb1 change before blanking fb0; 0 = disable |

Setting a new timeout also resets the idle timer.

---

### STATE

Query the current Kronos operating mode.

```
Request:  STATE\n
Response: MODE=<n>\n
```

| Value | Mode |
|-------|------|
| 0 | Init / unknown |
| 1 | Setlist |
| 2 | Combi |
| 3 | Program |
| 4 | Sequence |
| 5 | Sampling |
| 6 | Global |
| 7 | Disk |

The daemon updates its mode state whenever a `BUTTON` command targets a mode-select button. It does not read mode state from the hardware directly; the value reflects the last mode change issued through the control port, defaulting to 0 (Init) on startup.

---

### VERSION

Query the daemon version and build ID.

```
Request:  VERSION\n
Response: VER=<version> BUILD=<build_id>\n
```

Example:

```
VER=1.7.14 BUILD=20260702-1.7.14\n
```

`BUILD` is set at compile time from the date and version string.

---

### SYSINFO

Query a snapshot of system metrics. The response is a multi-line block terminated by `OK\n`.

```
Request:  SYSINFO\n
Response: <key>=<value>\n ... OK\n
```

See [Section 11](#11-sysinfo-field-reference) for the full field reference.

Note: CPU percentage fields require two successive `SYSINFO` calls to be meaningful. The first call will report `-1` for all `CPU*_PCT` fields because there is no prior sample to compute a delta from. The second and subsequent calls report the average CPU utilisation since the previous call.

---

### MIDI_SEND

Inject raw MIDI bytes into the Kronos MIDI engine. Requires the MIDI injection module to be loaded.

```
Request:  MIDI_SEND <hex>\n
Response: OK\n                    (bytes injected)
          ERR MIDI_NOT_LOADED\n   (midi_bridge.ko not loaded)
          ERR BAD_HEX\n           (hex decode failed)
```

| Argument | Type | Description |
|----------|------|-------------|
| hex | string | Hex-encoded MIDI bytes (pairs, spaces allowed). Example: `90 3C 7F` (Note On, middle C, velocity 127) |

The hex string is decoded and written to `/proc/.midi_in` which passes the bytes directly to the Kronos MIDI receive function. Any valid MIDI message can be sent: note on/off, CC, program change, SysEx, etc. Maximum message size is 4096 bytes.

**Continuous Controller throttling.** If `hex` decodes to exactly one 3-byte Control Change message (status `0xBn`), the daemon rate-limits it per `(status, controller)` pair to at most one injection every 7 ms, keeping only the latest value seen; the final value is always delivered once the controller stops moving. This prevents a fast controller sweep (e.g. many rapid `MIDI_SEND` calls moving mod wheel or breath CC) from flooding OA's MIDI queue and delaying other MIDI stuck behind it. The daemon still replies `OK\n` immediately regardless of whether the byte was injected immediately or held for coalescing. Notes, pitch bend, SysEx, and any multi-byte message are never throttled and are injected as sent.

---

### SYSEX

Send a SysEx message and capture the response. The daemon injects the SysEx via the `midi_tcp` subprocess and monitors the MIDI output stream for the reply.

```
Request:  SYSEX <hex>\n
Response: SYSEX_RESP <hex>\n      (captured response as hex)
          ERR MIDI_NOT_LOADED\n   (midi_bridge.ko not loaded or capture unavailable)
          ERR BAD_SYSEX\n         (hex decode failed or first byte is not F0)
          ERR TIMEOUT\n           (no response received within timeout)
```

| Argument | Type | Description |
|----------|------|-------------|
| hex | string | Hex-encoded SysEx message (must start with `F0`, should end with `F7`). Spaces allowed between hex pairs. |

The capture timeout is approximately 5 seconds. The response includes the first complete F0...F7 SysEx message received from the Kronos after the request is sent, up to 65536 bytes. For SysEx request/response to work, **Global > MIDI > "Enable Exclusive" must be ON** on the Kronos, or SysEx messages are silently ignored.

**Small exchanges only.** `SYSEX` captures a *single* `F0...F7` block and caps at 65536 bytes, so it suits short request/response exchanges (mode request, current performance id, a name dump). To retrieve a **large** object (a full Set List is ~79 KB) or a multi-object bank dump, do **not** use `SYSEX` - send the request with `MIDI_SEND` and collect the resulting `0x73` Object Dump reply off the MIDI bridge stream (port 9875; see [Section 8](#8-midi-bridge---port-9875)), which streams SysEx incrementally with no size cap.

**Asynchronous delivery.** The `midi_tcp` subprocess forwards all MIDI output from the Kronos (note events, CC, SysEx, real-time bytes) as a continuous stream without buffering or request/response pairing. Non-SysEx MIDI events such as note-on/note-off may arrive interleaved with the SysEx response. Delivery order within the stream is preserved (TCP guarantees this), but clients must be prepared to discard or buffer non-SysEx messages that arrive while waiting for a response. Response correlation is by payload content: Korg SysEx messages carry a manufacturer ID (0x42), model ID (0x68 for Kronos), and a function code that identifies the message type - match on those rather than on timing or position in the stream.

---

### MIDI_STATUS

Query the state of the MIDI injection subsystem.

```
Request:  MIDI_STATUS\n
Response: MIDI_LOADED=<0|1>\n
          MIDI_IN=<0|1>\n
          MIDI_CAPTURE=<0|1>\n
          OK\n
```

| Field | Description |
|-------|-------------|
| `MIDI_LOADED` | 1 if `midi_bridge.ko` was loaded successfully |
| `MIDI_IN` | 1 if `/proc/.midi_in` is open and writable |
| `MIDI_CAPTURE` | 1 if `/proc/.midi_ring` is available and MIDI output capture is active (channel messages and SysEx) |

---

## 8. MIDI bridge - port 9875

The MIDI bridge is a standalone TCP service (`midi_tcp` subprocess) that runs alongside the main daemon. Unlike the stream and control ports it requires **no authentication** and is accessible from any host on the network.

### 8.1 Connection

Connect to TCP port 9875. The port is not configurable. Up to 8 clients may be connected simultaneously; connections beyond that limit are rejected with no response. `TCP_NODELAY` is set on all client sockets.

### 8.2 Outbound - Kronos MIDI output -> client

Once connected the server streams raw MIDI bytes captured from the Kronos MIDI output engine. There is no framing or length prefix - the byte stream follows the standard MIDI byte protocol and clients are responsible for parsing it into complete messages.

**Captured message types:**

| Type | Status byte | Notes |
|------|-------------|-------|
| Note Off | `0x8n` | |
| Note On | `0x9n` | |
| Polyphonic Aftertouch | `0xAn` | |
| Control Change | `0xBn` | |
| Program Change | `0xCn` | |
| Channel Aftertouch | `0xDn` | |
| Pitch Bend | `0xEn` | |
| System Common | `0xF1`-`0xF6` | MTC, Song Position, Song Select, Tune Request |
| SysEx | `0xF0` ... `0xF7` | Korg SysEx: responses, and bulk data dumps (Set List / All Data / Object Dumps) |
| Real-time | `0xF8`-`0xFF` | Clock, Start, Continue, Stop, Active Sensing - only if MIDI clock output is enabled on the Kronos |

**What is NOT captured:**

Internal Korg STG message-bus traffic (e.g. patch-name parameter broadcasts) never reaches the physical MIDI output layer and will not appear in the stream. These were incorrectly captured in versions prior to 1.7.4.

**Generic, destination-agnostic capture.**

The stream is a **single generic MIDI-out feed**: it carries everything the Kronos transmits - live performance (notes, CC, program/combi change, aftertouch, pitch bend, system-common) **and** SysEx responses and bulk data dumps - regardless of which physical destination (DIN or USB) a message is bound for, and the client never has to know or choose. Concretely, the kernel module taps OA's shared performance queue **once** plus every out-port's per-destination dump queue, and merges them into this one stream.

- **No duplication.** Each performance message appears **exactly once**, even when both DIN and USB outputs are enabled - they share one source queue, read once.
- **Bulk data dumps are captured** to any destination. A dump sent to **USB** streams at memory speed (~800 KB/s+); one sent to **DIN** arrives at 5-pin MIDI speed (~3.6 KB/s) - the same bytes, just slower off the wire. Either way it lands on this one stream with no size cap.
- OA may still emit its own genuine copies of some messages (e.g. a program change echoed per active timbre). Those are real MIDI from the Kronos, not a capture artifact, and are passed through unchanged.

*(Real-time footprint, since 1.9.2: this continuous tap accesses the codec's real-time-critical memory, so it depends on the daemon being pinned off the audio engine's CPU core - see "Ring buffer and backpressure" below. A `tap_shared=0` module option drops live-performance capture and reads the codec queues only inside a request-scoped dump window, for a minimal-footprint dump-only build; it is **not** the default.)*

**Migration note (pre-1.9.0):** early revisions captured per physical port and doubled every event, and this doc once told clients to deduplicate consecutive identical messages within ~5 ms. **That dedup logic must be removed** - treat every message as a distinct event.

**Running status:**

Channel and system-common messages are reassembled by the `midi_tcp` parser and broadcast as complete, fully-formed messages (each with an explicit status byte), so clients do not need to implement running-status tracking for those.

**SysEx streaming:**

SysEx is the exception. Rather than buffer a whole `F0...F7` block and broadcast it at once, `midi_tcp` **streams SysEx bytes to clients incrementally, in chunks of up to 1 KB** (and flushes again at `F7`). This lets an arbitrarily large object - a full Set List dump is ~79 KB - traverse the bridge with **no size cap**, and keeps a client's activity/keepalive logic fed throughout a multi-second transfer. Only the first chunk carries the `F0` and only the last carries the `F7`; clients reassemble the message across chunk (and TCP) boundaries exactly as they already parse the raw byte stream (Section 8.2). *(Before this change SysEx was buffered whole and capped at 64 KB, which silently truncated any larger object and dropped its trailing `F7`, so a Set List dump never completed on the client.)*

**Ring buffer and backpressure:**

The kernel module (`midi_bridge.ko`) captures MIDI out by attaching as an additional reader on OA's own transmit queues - it does **not** patch OA code. Captured bytes are staged in a 64 KB circular ring and drained to `midi_tcp` when it reads. Data written to the ring before any client is connected is discarded when the first client connects (the read cursor is reset). Draining is **best-effort**: if the client falls behind, the module skips its own copy forward rather than hold OA's transmit queue back - so a slow or stalled client can **never** throttle or corrupt the Kronos's real MIDI output; the only casualty is the client's own captured copy of the bytes it was too slow to read. Dropped bytes are counted in `overflow` in `/proc/.midi_ports` and should read `0` in normal use. A full USB Set List / All Data dump (multiple MB) has been verified to capture losslessly (`overflow=0`) at ~800 KB/s.

**Real-time safety (1.9.2).** OA's transmit queues live in the codec's real-time-critical memory, so continuously tapping them is safe only because the daemon and its `midi_tcp` subprocess are pinned to a CPU core away from the audio engine's core (`sched_setaffinity`). Two related operational notes for host software:
- **MIDI is unavailable for roughly the first minute after power-on.** `midi_bridge` is loaded only after the Kronos UI has finished initializing (the port `9875` listener does not exist before then); a connection attempt before that simply fails and should be retried. `MIDI_STATUS` (Section 7) reports readiness.
- The core-pinning is internal and needs no client action, but it is why streaming + continuous MIDI capture can run together without disturbing the synth. *(Earlier builds let the OS schedule streaming onto the audio core; during the boot-settling window that starved the real-time engine and froze the UI - the whole reason capture is gated on both the core pinning and the deferred, post-UI load.)*

*(Pre-1.9.0 revisions inline-hooked OA's output drain and used a shared spin-trylock that dropped a whole batch whenever the reader held the lock; under a dense bulk dump this punched holes into the SysEx byte stream and corrupted large objects. The current reader-tap design has neither hazard.)*

### 8.3 Inbound - client MIDI -> Kronos

Write raw MIDI bytes to the TCP connection to inject them into the Kronos MIDI receive engine (`MidiInPortGeneric7Receive`). Any valid MIDI message type is accepted: note on/off, CC, program change, pitch bend, SysEx, real-time, etc.

Requests are injected into the Kronos's **USB** input port, so the Kronos routes its reply back out the fast USB path - a Set List dump returns in well under a second rather than the ~25 s a DIN-routed reply would take. The reply appears on the port-9875 outbound stream (Section 8.2); the normal dump workflow is *inject the request, then collect the reply off that stream*. (In the non-default `tap_shared=0` dump-only build, injecting is also what opens the outbound capture window.)

Maximum single write: 4096 bytes (kernel module buffer limit). Larger payloads must be split across multiple writes.

### 8.4 Hub semantics

All connected clients share the same single input and output channel:

- Kronos MIDI output is broadcast to **all** connected clients simultaneously via `send(..., MSG_DONTWAIT)`. A blocked or slow client does not affect delivery to other clients (the send is non-blocking; data to a full-buffer client is dropped for that delivery). Each client socket is given a **256 KB send buffer** (`SO_SNDBUF`) so a momentary client stall cannot fill the buffer and drop a chunk in the middle of a large SysEx dump.
- MIDI injected by any client is delivered to the Kronos. Other clients do **not** receive an echo of injected data unless the Kronos itself echoes it back as MIDI output.

### 8.5 Ring cursor reset

The ring cursor is reset (pre-connection buffered data discarded) when the **first** client connects to an otherwise empty hub. Clients that connect while others are already active begin receiving from the current write position and may receive mid-message data if they arrive during an active capture burst. Clients should implement standard MIDI re-sync logic (scan forward for the next status byte with bit 7 set if the first byte received is a data byte).

### 8.6 Retrieving program & combi names (Kronos SysEx notes)

Pulling the names of every program/combi in a bank is a common tooling task, and the Kronos firmware has a **non-obvious asymmetry** that is easy to misdiagnose. These notes apply to any client (this daemon or otherwise) that requests Object Dumps over MIDI; they are properties of the **Kronos**, not of the bridge. All are hardware-verified (2026-07-08). Names are carried in Object Dump replies (`0x73`) at data offset 0 (24 bytes, 8->7 packed) for both the full object (`0x00` Program / `0x01` Combi) and the name-only object (`0x13` Program Name / `0x12` Combi Name).

**Bank numbers** (KRONOS MIDI Implementation, *2 bank map):

| Object | Preset / read-only banks | USER-writable banks |
|--------|--------------------------|---------------------|
| Program / Program Name (`0x00`/`0x13`) | `0x00`-`0x05` (INT-A...F), `0x10`-`0x1A` (GM, g(1)-g(9), g(d)) | `0x40`-`0x4D` (USER-A...G, AA...GG) |
| Combi / Combi Name (`0x01`/`0x12`) | `0x00`-`0x06` (INT-A...G) | `0x40`-`0x46` (USER-A...G) |

**The asymmetry - the whole-bank name enum is preset-only.** The **Dump Bank Request** (`func 0x77`, e.g. `F0 42 3g 68 77 13 <bank> F7`) works for the **preset** banks (INT, GM/g) and streams all 128 names in ~20 ms, but for every **USER-writable** bank it returns a **Reply (`func 0x24`) with code 4 "target object not found"** - even when the bank is fully populated. This is a firmware limitation of the name-only enum, not an empty bank and not a permissions issue.

- There is **no** "banks per session" throttle. A persistent belief that the Kronos rejects everything after ~13 banks was a misreading of *this* preset-only reject (preset banks dump, USER banks reject). Preset `0x77` enums and USER per-object fetches (below) both stream reliably with no session cap.

**The workaround - request each object individually with `func 0x72`.** The single **Object Dump Request** (`func 0x72`) works for **every** bank, including USER:

```
Request (one name):  F0 42 3g 68 72 <obj> <bank> <idH> <idL> F7
   obj  = 0x13 (Program Name) or 0x12 (Combi Name)
   idH  = (index >> 7) & 0x7F,  idL = index & 0x7F     (index 0..127)
Reply:               F0 42 3g 68 73 <obj> <bank> <idH> <idL> <version> <name...> F7
```

Loop `index` 0...127 to pull a whole USER bank. Empty slots reply with a **blank-named** object (not a reject), so all 128 indices respond for a bank that exists.

**Pacing is mandatory - never burst.** Injecting a bank's worth of requests back-to-back **overruns the Kronos MIDI-in**: it drops *every* reply and can corrupt a request in flight (a lost `F7`), which makes the Kronos pop a user-facing **"MIDI Receiving Error"** dialog. Two safe patterns, both verified at a clean 128/128:

- **Batched (recommended):** concatenate ~32 `0x72` requests into one injection (keep it under the ctrl port's ~2 KB line limit if using `MIDI_SEND`; the bridge's inbound path caps a single write at 4096 bytes - see Section 8.3), then **wait for that batch's `0x73` replies to go idle (~350 ms) before sending the next batch**. ~4 batches pull a 128-object bank in ~2 s.
- **Paced singles:** one `0x72` request every ~10 ms.

Also avoid **connection churn**: if you inject via the one-shot control port (`MIDI_SEND`, one TCP connection per command - Section 6.1), one-connection-per-request is ~2688 connections for a full 21-bank user sweep and can overwhelm the daemon. Batching (~4 injections/bank) or a persistent control session (Section 6.2) avoids this.

**Absent vs. empty vs. rejected** when driving a sweep: match replies by content (Section 7 `SYSEX` note) - a `0x73` with the requested `obj`/`bank` is a name (blank or not); a `0x24` code 4 with no `0x73` for that bank means the whole-bank enum was refused (you asked with `0x77` on a USER bank - switch to `0x72`). A bank that yields no reply to a full first batch of `0x72` requests (after a generous wait) is genuinely absent/nonexistent.

---

## 9. Button name reference

Button names are case-sensitive and must be uppercase. The `code` column is the button's flat NKS4 hardware scan code (0-127) - see [Section 7.0](#70-front-panel-injection-architecture-as-of-1100) for what that means and how it was obtained. Every code below was captured directly off a real unit's NKS4 test/calibration mode (one physical press per button) and independently cross-checked against `OA.ko`'s own `ButtonPressHandler` disassembly.

**`INC`/`DEC`** (front-panel value +/-, codes 51/52) were captured in a dedicated follow-up pass after the main table, independently of the "PanelSW+/-" label an earlier capture pass used for the same two codes - two captures agreeing on the same codes for what both the C# and Python clients already call `INC`/`DEC` confirms it's one physical button pair with one correct name, not two.

See [`nks4_firmware_crossref.md`](nks4_firmware_crossref.md) for a cross-check of this table against the panel controller's own firmware — including a naming curiosity (`SETLIST` is internally called `"Live"`), confirmed bi-color LEDs on the Sampling and Seq transport buttons, and an unconfirmed possible gap around KARMA module selection.

### Navigation

| Name | code | Description |
|------|------|-------------|
| `EXIT` | 8 | Exit button |
| `ENTER` | 23 | Enter / confirm button |

### Value control

| Name | code | Description |
|------|------|-------------|
| `INC` | 51 | Increment the currently selected value |
| `DEC` | 52 | Decrement the currently selected value |

### Mode select

| Name | code | Description |
|------|------|-------------|
| `SETLIST` | 7 | Setlist mode |
| `COMBI` | 1 | Combi mode |
| `PROGRAM` | 2 | Program mode |
| `SEQUENCE` | 3 | Sequence mode |
| `SAMPLING` | 4 | Sampling mode |
| `GLOBAL` | 5 | Global mode |
| `DISK` | 6 | Disk mode |

### Utility

| Name | code | Description |
|------|------|-------------|
| `HELP` | 9 | Help button |
| `COMPARE` | 10 | Compare button |
| `RESET` | 75 | Reset Controls button |

### Numeric pad

| Name | code | Description |
|------|------|-------------|
| `NUM0` - `NUM9` | 11, 12-20 | Numeric keys 0-9 (0 is code 11, then 1-9 are codes 12-20 in order) |
| `NUM_DASH` | 21 | Numeric dash / minus |
| `NUM_DOT` | 22 | Numeric dot / decimal |

### Mix Play buttons

`MP1` through `MP8` = codes 58-65 in order.

### Mix Select buttons

`MS1` through `MS8` = codes 66-73 in order.

### Bank buttons

| Name | code | Description |
|------|------|-------------|
| `BANK_IA` - `BANK_IG` | 24-30 | Internal banks A through G, in order |
| `BANK_UA` - `BANK_UG` | 31-37 | User banks A through G, in order |

### Sequencer

| Name | code | Description |
|------|------|-------------|
| `SEQ_PAUSE` | 38 | Pause |
| `SEQ_REW` | 39 | Rewind |
| `SEQ_FF` | 40 | Fast forward |
| `SEQ_LOCATE` | 41 | Locate / return to start |
| `SEQ_REC` | 42 | Sequencer record |
| `SEQ_START` | 43 | Sequencer start / stop (same physical button drives both) |
| `TAP_TEMPO` | 44 | Tap tempo |

### Sampling

| Name | code | Description |
|------|------|-------------|
| `SMPL_REC` | 45 | Sampling record |
| `SMPL_START` | 46 | Sampling start |

### Channel strip / control surface

| Name | code | Description |
|------|------|-------------|
| `MIX_KNOBS` | 74 | Mixer Knobs selector |
| `SOLO` | 76 | Solo (fires on release; the daemon sends press+release so the release event triggers it) |
| `MODULE_CONTROL` | 47 | Module Control |
| `KARMA_ONOFF` | 48 | Karma On/Off |
| `KARMA_LATCH` | 49 | Karma Latch |
| `DRUM_TRACK` | 50 | Drum Track select |
| `TIMBRE_TRACK` | 53 | Timbre/Track select |
| `AUDIO_TRACK` | 54 | Audio select |
| `EXT_TRACK` | 55 | Ext select |
| `RTKNOBS_KARMA` | 56 | "RT Knobs/Karma" control-surface page select |
| `TONE_ADJUST` | 57 | Tone Adjust |
| `SW1` | 77 | Front-panel switch 1 |
| `SW2` | 78 | Front-panel switch 2 |

---

## 10. Analog device code reference

The `SLIDER`, `KNOB`, `VSLIDER`, `JOYSTICK`, `VECTOR`, `RIBBON` (X axis), `AFTERTOUCH`, `PEDAL`, and `FOOTSWITCH` commands (Section 7) are all hardware-confirmed against a real unit and accept a direct jump to any target value. `DAMPER` and `TEMPO` are also hardware-confirmed, and take the same `byte0 = value * 2` input, but neither can be jumped to directly - a large single-step change produces inconsistent, non-repeatable results, while a smooth monotonic ramp through every intermediate value is clean and precisely reproducible (confirmed both ways for `TEMPO`: identical results ascending and descending). Both commands ramp internally now, so this is handled for the caller - see their own sections below. `RIBBON`'s Z axis remains the one untested exception - its device code is real (a confirmed dispatch entry), but no command has verified how its value is interpreted. Static analysis of `OA.ko`'s own `AnalogControllerHandler` dispatch tables (three separate jump tables, each entry a real relocation to a named `CSTGControllerInfo::Analog*Handler` method) identified the **full** `eSTGAnalogDeviceCode` range with high confidence, and every one of these was independently cross-checked against the official Kronos Operation Guide's front-panel control list to confirm it's a real, standard control - not inert code shared from another product in the same codebase with no hardware behind it on this unit. Device code 24 remains deliberately unexposed - a context-dependent effects-rack edit, not a fixed control.

| Code | Handler | Control | Status |
|------|---------|---------|--------|
| 1 | `AnalogJoystickXHandler` | Joystick (item 12 in the Kronos manual), left/right - pitch bend | **Hardware-confirmed** - circular sweep test |
| 2 | `AnalogJoystickYHandler` | Joystick (item 12), forward/back - vibrato/wah | **Hardware-confirmed** - circular sweep test |
| 3 | `AnalogRibbonXHandler` | Ribbon controller (item 13), finger position | **Hardware-confirmed** - center/max/min sweep |
| 4 | `AnalogRibbonZHandler` | Ribbon controller (item 13), second axis - real dispatch entry confirmed, but which physical property it reads is not verified (a ribbon's "Z" axis commonly means touch pressure, unconfirmed here) | Command implemented, scaling unverified |
| 5 | `AnalogVectorXHandler` | **Vector Joystick** (item 9) - Vector Synthesis. A genuinely separate physical joystick from codes 1/2, not a duplicate | **Hardware-confirmed** - circular sweep test |
| 6 | `AnalogVectorYHandler` | Vector Joystick (item 9), other axis | **Hardware-confirmed** - circular sweep test |
| 7 | `AnalogAftertouchHandler` | Keybed channel aftertouch | **Hardware-confirmed** - 0/half/full/half/0 sweep |
| 8-15 | (per-page `knobHandlers`) | RT Knobs 1-8 | **Hardware-confirmed** - see `KNOB` |
| 16-23 | (per-page `sliderHandlers`) | Sliders 1-8 | **Hardware-confirmed** - see `SLIDER` |
| 24 | (context handler) | Effects-rack parameter edit - only reachable in an effects-editing UI context, not a fixed physical control; do not treat as a stable device | Identified, not hardware-tested |
| 25 | `AnalogValueSliderHandler` | Value Slider | **Hardware-confirmed** - see `VSLIDER` |
| 26 | `AnalogTempoHandler` | Tempo - non-linear 0-127 to ~40-300bpm curve, requires a gradual ramp (same behaviour class as `DAMPER`) | **Hardware-confirmed** - see `TEMPO` |
| 27 | `AnalogFootPedalHandler` | Rear-panel assignable PEDAL jack | **Hardware-confirmed** - 0/half/full/half/0 sweep, large jumps fine |
| 28 | `AnalogFootSwitchHandler` | Rear-panel assignable foot SWITCH jack | **Hardware-confirmed** - on/off tap |
| 29 | `AnalogDamperHandler` | Rear-panel DAMPER jack (sustain / half-damper) | **Hardware-confirmed**, but requires a gradual ramp - see `DAMPER` command notes |
| 30 | `SetControllerAssignment` | **Not a physical control** - assigns what a controller maps to, a system action rather than a value reading. Out of scope for this table | Identified, different kind of action |

"Identified" means: a real, named dispatch table entry in `OA.ko` reaching a plausible handler for a control confirmed to physically exist on this hardware by the Kronos manual - high confidence, but no injected value has been confirmed to produce the expected on-screen/audible result the way the hardware-confirmed group has.

---

## 11. SYSINFO field reference

All fields are plain ASCII decimal unless otherwise noted. Fields that cannot be read from the system are omitted from the response.

| Field | Type | Description |
|-------|------|-------------|
| `UPTIME` | integer | System uptime in seconds (from `/proc/uptime`) |
| `LOAD` | `f f f` | 1, 5, and 15-minute load averages, two decimal places (from `/proc/loadavg`) |
| `MEM_TOTAL_KB` | integer | Total RAM in kilobytes |
| `MEM_FREE_KB` | integer | Free RAM in kilobytes |
| `MEM_AVAIL_KB` | integer | Estimated available RAM (free + buffers + cached) in kilobytes |
| `CPU_PCT` | integer | Aggregate CPU utilisation percentage since last SYSINFO call; -1 on first call |
| `CPU0_PCT` - `CPU3_PCT` | integer | Per-core utilisation percentage; -1 on first call |
| `AUDIO_SR` | integer | Audio sample rate in Hz |
| `AUDIO_OUT_CH` | integer | Number of audio output channels |
| `AUDIO_RTO` | integer | Audio round-trip overrun count (from `/proc/KorgUsbAudio`) |
| `AUDIO_MIDI_RT` | integer | MIDI output real-time call count |
| `DISK_FREE_MB` | integer | Free space on `/korg/rw` in megabytes |
| `DISK_TOTAL_MB` | integer | Total size of `/korg/rw` in megabytes |
| `RW2_FREE_MB` | integer | Free space on `/korg/rw2` (second internal SSD) in megabytes |
| `RW2_TOTAL_MB` | integer | Total size of `/korg/rw2` in megabytes |
| `USB0_MNT` | string | Mount point of first USB drive |
| `USB0_FREE_MB` | integer | Free space on first USB drive in megabytes |
| `USB0_TOTAL_MB` | integer | Total size of first USB drive in megabytes |
| `USB1_MNT` | string | Mount point of second USB drive (if present) |
| `USB1_FREE_MB` | integer | Free space on second USB drive in megabytes |
| `USB1_TOTAL_MB` | integer | Total size of second USB drive in megabytes |
| `USB_COUNT` | integer | Number of mounted USB drives detected (0-2) |
| `TEMP1` - `TEMP3` | integer | Hardware temperature sensor readings in degrees Celsius |
| `FAN1_RPM` | integer | Fan speed in RPM |
| `MODE` | integer | Current Kronos mode (same values as STATE command) |

The response is terminated with:

```
OK\n
```

---

## 12. Authentication internals

Authentication is attempted in priority order. The first backend that recognises the username determines the result.

### 12.1 KronosNet.conf

File: `/korg/rw/Startup/KronosNet.conf`

Line 1: username (plain text)
Line 2: password (plain text)

This is the credential store managed by the Kronos UI (the network/FTP user). If the submitted username matches line 1, authentication either succeeds (passwords match) or fails (passwords differ) and no further backends are tried.

### 12.2 PublicID fallback

If `KronosNet.conf` is missing or does not contain the submitted username, the daemon accepts username `kronos` with the device's PublicID as the password. The PublicID is the dashed form shown in the Kronos UI (Global > Basic, Menu > Display Public ID), e.g. `AA-BB-CC-DD-EE-FF-00-11`. Dashes are optional - the daemon strips them before comparing, so both `AA-BB-CC-DD-EE-FF-00-11` and `AABBCCDDEEFF0011` are accepted. Any other username or password is rejected.

The PublicID is read from `/proc/id` (created by `GetPubIdMod.ko` at boot) and is unique per device. It is visible to the device owner but not guessable by an external attacker.

This fallback is intended as an emergency recovery path for screen connect only. It does not grant FTP access. It covers cases where `KronosNet.conf` is absent - for example on a Nautilus where the file may not exist or may be named differently. No directory flag or configuration is required to enable it.

### 12.3 Access log

Every authentication attempt (success or failure) is appended to `/korg/rw/screenremote/access.log` with a timestamp, client IP, and outcome:

```
2024-09-01 12:34:56  192.168.1.42   ACCEPTED
2024-09-01 12:35:01  192.168.1.99   REJECTED  user=unknown: user not found
```

---

## 13. Error handling and disconnection

### Stream port

- If any `write` on the stream socket fails, the client is dropped silently and the daemon returns to listening for new connections.
- On client drop: `client_fd` is closed, shadow frame is invalidated, and the allowed control IP is cleared (reverting to allow-any).
- There is no keepalive or ping mechanism. A stale client is only detected when a write fails.

### Control port

- Invalid command arguments return `ERR\n`; unrecognised command names return no response and the connection is closed (one-shot mode) or silently ignored (persistent mode).
- A persistent control connection that closes or errors is detected on the next `select` readable event; the daemon clears `ctrl_fd` and the CPU delta baseline (`g_si_prev_valid`) is reset.

### Signal handling

`SIGTERM` and `SIGINT` set a flag that causes the main loop to exit cleanly after the current iteration. On exit the daemon closes all sockets, zeros `/dev/fb0` if the mirror is active, and restores the fbcon text cursor.

`SIGPIPE` is ignored; write errors on network sockets are detected by the return value instead.

---

## 14. Implementation limits

| Limit | Value | Notes |
|-------|-------|-------|
| Maximum FPS | 15 | Hard-coded; client requests above this are clamped |
| Maximum username length | 64 bytes | Enforced during handshake |
| Maximum password length | 128 bytes | Enforced during handshake; buffer is zeroed after auth |
| Maximum concurrent stream clients | 1 | New connection replaces existing client |
| Maximum concurrent control connections | 1 persistent + transient one-shots | New CTRL_PERSIST replaces previous persistent session |
| Control line buffer | 2048 bytes | Persistent mode; lines longer than this are silently truncated |
| Handshake timeout | 5 seconds | Applied to the full client hello read |
| Screensaver sample interval | 5 seconds | How often fb1 is sampled for change detection |
| Screensaver sample points | 16 pixels | Evenly spaced across the framebuffer |
| PackBits RLE literal run max | 128 bytes | Standard PackBits limit |
| PackBits RLE repeat run max | 128 repetitions | Standard PackBits limit |
| fb0 poll for `/dev/fb1` at startup | 30 seconds | 300 x 100 ms; daemon exits if fb1 never appears |
| vkbd.ko open poll at startup | 2 seconds | 20 x 100 ms; falls back to uinput if /proc/.vkbd never appears |
| midi_bridge.ko open poll at startup | 2 seconds | 20 x 100 ms; MIDI disabled if /proc/.midi_in never appears |
| nks4_inject.ko open poll at startup | 2 seconds | 20 x 100 ms; front-panel injection (BUTTON/CHORD/TOUCH*/WHEEL/SLIDER/KNOB/VSLIDER) unavailable if /proc/.nks4inject never appears |
| midi_tcp connect poll at startup | 6 seconds | 30 x 200 ms; SysEx capture unavailable if connection fails |
| CHORD max buttons | 8 | `name1`..`name8`; a 9th name is silently ignored (parser stops filling the array) |
| CHORD hold_ms range | 0 - 5000 ms | Snapped, not rejected - see Section 7.0 |
| SLIDER / KNOB index range | 1 - 8 | Snapped, not rejected |
| SLIDER / KNOB / VSLIDER value range | 0 - 127 | Snapped, not rejected |
| Touch injection minimum spacing | 30 ms | `TOUCH_MIN_INTERVAL_MS`; enforced between every `TOUCH`/`TOUCH_DOWN`/`TOUCH_MOVE`/`TOUCH_UP` injection - see `TOUCH`'s pacing note in Section 7 |
| PADCHORD minimum hold duration | 80 ms | `PADCHORD_MIN_HOLD_MS`; minimum time between a bridged pad trigger and its release - see `PADMAP`'s "Minimum hold duration" note in Section 7 |
| PADMAP pad count | 8 | `pad_index` 0-7; `PADMAP`/`PADCHORD` reject/clamp outside this range |
| REGION rectangle size | 8192 pixels | `(x1-x0+1) * (y1-y0+1)`; larger requests return `ERR\n` rather than truncating - see `REGION` in Section 7 |
| PALETTE response size | 768 bytes decoded (1536 hex chars) | 256 palette entries x 3 bytes RGB |
| MIDI_SEND max message size | 4096 bytes | Limited by the kernel module's static buffer |
| MIDI_SEND CC throttle interval | 7 ms | Minimum spacing between injected Control Change messages sharing the same (status, controller) |
| MIDI_SEND CC throttle tracked controllers | 32 | Concurrent (status, controller) pairs tracked; a 33rd distinct controller bypasses throttling |
| SysEx capture buffer (`SYSEX` command) | 65536 bytes | Max response for a single `SYSEX` command; larger objects must be collected off the MIDI bridge stream (Section 8), which has no cap |
| SysEx capture timeout | ~5 seconds | Initial 5 s recv timeout, then 1 s for trailing data |
| MIDI bridge max clients | 8 | Connections beyond this are rejected with no response |
| MIDI bridge SysEx stream chunk | 1024 bytes | SysEx is flushed to clients in <=1 KB chunks; large objects (e.g. a ~79 KB Set List) stream with no total size cap |
| MIDI bridge output ring | 16384 bytes | Kernel lock-free single-producer/single-consumer ring; drops only on genuine overflow (unread data preserved); overflow byte count in `/proc/.midi_ports` as `ring_overflow_bytes` |
| MIDI bridge client send buffer | 262144 bytes | `SO_SNDBUF` per client socket, so a stalled client can't drop a chunk mid-dump |
| MIDI bridge inbound buffer | 4096 bytes | Per-write maximum for MIDI injection; split larger payloads across writes |
| Touch calibration: `touch_x_offset` | 10 | Pixels added to x before ADC scaling |
| Touch calibration: `touch_x_range` | 813 | Pixel span mapped to ADC 0-255 (horizontal) |
| Touch calibration: `touch_y_offset` | 20 | Pixels added to y before ADC scaling |
| Touch calibration: `touch_y_range` | 638 | Pixel span mapped to ADC 0-255 (vertical) |
