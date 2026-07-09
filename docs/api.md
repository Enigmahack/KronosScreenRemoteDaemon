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
10. [SYSINFO field reference](#10-sysinfo-field-reference)
11. [Authentication internals](#11-authentication-internals)
12. [Error handling and disconnection](#12-error-handling-and-disconnection)
13. [Implementation limits](#13-implementation-limits)

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

---

## 7. Control port - command reference

All commands respond with `OK\n` on success or `ERR\n` on invalid arguments, except where noted.

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
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| x | integer | 0 to width-1 | Horizontal pixel coordinate |
| y | integer | 0 to height-1 | Vertical pixel coordinate |

Coordinates are clamped to the framebuffer bounds. Events are written to `/dev/rtf5` as the Kronos touchscreen FIFO protocol (see TOUCH_DOWN/TOUCH_UP for the encoding).

---

### TOUCH_DOWN

Send a pen-down event only.

```
Request:  TOUCH_DOWN <x> <y>\n
Response: OK\n
```

Arguments are the same as `TOUCH`. Use `TOUCH_DOWN` followed by zero or more `TOUCH_MOVE` calls and then `TOUCH_UP` to simulate a drag gesture.

---

### TOUCH_MOVE

Send a pen-move (drag) event.

```
Request:  TOUCH_MOVE <x> <y>\n
Response: OK\n
```

Arguments are the same as `TOUCH`. Should be sent between a `TOUCH_DOWN` and `TOUCH_UP`.

---

### TOUCH_UP

Send a pen-up event.

```
Request:  TOUCH_UP <x> <y>\n
Response: OK\n
```

Arguments are the same as `TOUCH`.

#### Touch event wire encoding (internal, `/dev/rtf5`)

Each touch event is a 20-byte packet of five `uint32_t` values:

```
pkt[0] = 0x00010014   (fixed header)
pkt[1] = 0x00000000   (reserved)
pkt[2] = 0x00000011   (fixed: touch device ID)
pkt[3] = event_type   (1 = pen-down, 2 = pen-up, 3 = pen-move)
pkt[4] = v_adc | (h_adc << 8)
```

ADC values are computed from pixel coordinates using configurable touch calibration parameters (see [Section 13](#13-implementation-limits) for config keys):

```
cx = x + touch_x_offset          (default: 10)
cy = y + touch_y_offset          (default: 20)
h_adc = clamp(round(cx * 255 / touch_x_range), 0, 255)
v_adc = clamp(round(cy * 255 / touch_y_range), 0, 255)
```

With defaults (`touch_x_offset=10`, `touch_x_range=813`, `touch_y_offset=20`, `touch_y_range=638`), pixel (0,0) maps to approximately ADC (3,8) and pixel (799,599) maps to approximately ADC (254,247). The calibration parameters can be adjusted in `screenremote.cfg` if the touch response is misaligned on a particular unit.

---

### BUTTON

Press and release a named front-panel button. See [Section 9](#9-button-name-reference) for the full list of button names.

```
Request:  BUTTON <name>\n
Response: OK\n  (button found)
          ERR\n (button name not recognised)
```

| Argument | Type | Description |
|----------|------|-------------|
| name | string | Button name from the button table (case-sensitive, uppercase) |

The event is delivered as two 20-byte packets on `/dev/rtf5` - press (`0x7f`) then release (`0x00`). Mode-select buttons (SETLIST, COMBI, etc.) also update the daemon's internal mode state so `STATE` queries reflect the change immediately.

#### Button event wire encoding (internal, `/dev/rtf5`)

```
pkt[0] = 0x00010014   (fixed header)
pkt[1] = 0x00000000   (reserved)
pkt[2] = button.dev   (device group; see button table)
pkt[3] = button.code  (button code within the device group)
pkt[4] = 0x7f         (press) or 0x00 (release)
```

---

### CHORD

Press two or more buttons as a chord: buttons are pressed left-to-right, then released right-to-left, so all buttons are held simultaneously at the midpoint.

```
Request:  CHORD [<hold_ms>] <name1> <name2> [<name3> ... <name8>]\n
Response: OK\n  (all buttons found)
          ERR\n (any button name not recognised)
```

| Argument | Type | Description |
|----------|------|-------------|
| hold_ms | integer | Optional hold duration in milliseconds (0-5000) before releasing. If omitted, buttons are released immediately after all are pressed. |
| name1 | string | First button (held down first, released last) |
| name2 | string | Second button |
| name3..name8 | string | Optional additional buttons (up to 8 total) |

For N buttons, 2xN `/dev/rtf5` packets are sent: N presses in order, then N releases in reverse order. If `hold_ms` is specified, the daemon sleeps that duration between the press and release phases (blocks the main loop for the duration).

---

### WHEEL

Send one data wheel tick.

```
Request:  WHEEL CW\n
          WHEEL CCW\n
Response: OK\n
          ERR\n (direction not CW or CCW)
```

| Argument | Value | Description |
|----------|-------|-------------|
| CW | clockwise | Increment (positive direction) |
| CCW | counter-clockwise | Decrement (negative direction) |

#### Wheel event wire encoding (internal, `/dev/rtf5`)

The wheel uses a 16-byte (4 x `uint32_t`) packet:

```
pkt[0] = 0x00010010   (fixed header, 16-byte packet)
pkt[1] = 0x00000000   (reserved)
pkt[2] = 0x0000000d   (wheel device ID)
pkt[3] = 0x00000100   (CW)  or  0x0000FF00 (CCW)
```

---

### SLIDER

Set the position of a CC slider or RT knob (1-8).

```
Request:  SLIDER <n> <value>\n
Response: OK\n
          ERR\n (n not 1-8, value not 0-127, or parse failure)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| n | integer | 1-8 | Controller index (1 = leftmost slider/knob) |
| value | integer | 0-127 | Absolute position |

The physical effect depends on the active Control Assign page on the Kronos. In RT KNOBS/KARMA mode, `SLIDER n` moves knob n. In a slider-active mode, it moves slider n. In TIMBRE/TRACK mode the raw position event may not be interpreted (that mode uses processed 44-byte parameter packets internally).

#### Wire encoding (internal, `/dev/rtf5`)

```
pkt[0] = 0x00010014   (fixed header)
pkt[1] = 0x00000000   (reserved)
pkt[2] = 0x0000000e   (CC slider/knob device)
pkt[3] = n - 1        (0-based controller index)
pkt[4] = value        (0-127)
```

---

### VSLIDER

Set the value slider position.

```
Request:  VSLIDER <value>\n
Response: OK\n
          ERR\n (value not 0-127 or parse failure)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| value | integer | 0-127 | Absolute slider position |

The value slider changes the currently selected on-screen parameter proportionally. Moving to 0 sets the parameter minimum; 127 sets the maximum.

#### Wire encoding (internal, `/dev/rtf5`)

```
pkt[0] = 0x00010014   (fixed header)
pkt[1] = 0x00000000   (reserved)
pkt[2] = 0x0000000f   (value slider device)
pkt[3] = 0x00000009   (value slider code)
pkt[4] = value        (0-127)
```

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

See [Section 10](#10-sysinfo-field-reference) for the full field reference.

Note: CPU percentage fields require two successive `SYSINFO` calls to be meaningful. The first call will report `-1` for all `CPU*_PCT` fields because there is no prior sample to compute a delta from. The second and subsequent calls report the average CPU utilisation since the previous call.

---

### MIDI_SEND

Inject raw MIDI bytes into the Kronos MIDI engine. Requires the MIDI injection module to be loaded.

```
Request:  MIDI_SEND <hex>\n
Response: OK\n                    (bytes injected)
          ERR MIDI_NOT_LOADED\n   (midi_inject.ko not loaded)
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
          ERR MIDI_NOT_LOADED\n   (midi_inject.ko not loaded or capture unavailable)
          ERR BAD_SYSEX\n         (hex decode failed or first byte is not F0)
          ERR TIMEOUT\n           (no response received within timeout)
```

| Argument | Type | Description |
|----------|------|-------------|
| hex | string | Hex-encoded SysEx message (must start with `F0`, should end with `F7`). Spaces allowed between hex pairs. |

The capture timeout is approximately 5 seconds. The response includes the first complete F0...F7 SysEx message received from the Kronos after the request is sent, up to 65536 bytes. For SysEx request/response to work, **Global > MIDI > "Enable Exclusive" must be ON** on the Kronos, or SysEx messages are silently ignored.

**Small exchanges only.** `SYSEX` captures a *single* `F0…F7` block and caps at 65536 bytes, so it suits short request/response exchanges (mode request, current performance id, a name dump). To retrieve a **large** object (a full Set List is ~79 KB) or a multi-object bank dump, do **not** use `SYSEX` — send the request with `MIDI_SEND` and collect the resulting `0x73` Object Dump reply off the MIDI bridge stream (port 9875; see [Section 8](#8-midi-bridge---port-9875)), which streams SysEx incrementally with no size cap.

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
| `MIDI_LOADED` | 1 if `midi_inject.ko` was loaded successfully |
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
| SysEx | `0xF0` ... `0xF7` | Korg SysEx responses; drained from internal Block 5 ring |
| Real-time | `0xF8`-`0xFF` | Clock, Start, Continue, Stop, Active Sensing - only if MIDI clock output is enabled on the Kronos |

**What is NOT captured:**

Internal Korg STG message-bus traffic (e.g. patch-name parameter broadcasts) never reaches the physical MIDI output layer and will not appear in the stream. These were incorrectly captured in versions prior to 1.7.4.

**Duplicate events:**

The capture hook fires once per active physical output port. If the Kronos has more than one MIDI output port enabled (e.g. both DIN serial and USB), each note event will appear **twice** in the stream in rapid succession. Clients should either deduplicate consecutive identical messages within a short time window (~5 ms) or treat each copy as an independent event, depending on the application.

**Running status:**

Channel and system-common messages are reassembled by the `midi_tcp` parser and broadcast as complete, fully-formed messages (each with an explicit status byte), so clients do not need to implement running-status tracking for those.

**SysEx streaming:**

SysEx is the exception. Rather than buffer a whole `F0…F7` block and broadcast it at once, `midi_tcp` **streams SysEx bytes to clients incrementally, in chunks of up to 1 KB** (and flushes again at `F7`). This lets an arbitrarily large object — a full Set List dump is ~79 KB — traverse the bridge with **no size cap**, and keeps a client's activity/keepalive logic fed throughout a multi-second transfer. Only the first chunk carries the `F0` and only the last carries the `F7`; clients reassemble the message across chunk (and TCP) boundaries exactly as they already parse the raw byte stream (§8.2). *(Before this change SysEx was buffered whole and capped at 64 KB, which silently truncated any larger object and dropped its trailing `F7`, so a Set List dump never completed on the client.)*

**Ring buffer and backpressure:**

The kernel module buffers output in a 16 KB circular ring. Data written to the ring before any client is connected is discarded when the first client connects (ring cursor is reset). The ring is a **lock-free single-producer / single-consumer** design: the real-time capture hook (producer) and the `midi_tcp` reader (consumer) never share a lock, so the producer is never forced to drop data merely because the reader is mid-read. Data is dropped **only** on genuine overflow — a reader falling so far behind that the ring fills — and even then unread bytes are preserved (the newest output is lost, not the oldest). The running total of overflow-dropped bytes is exposed as `ring_overflow_bytes` in `/proc/.midi_ports` and should read `0` in normal use. *(Earlier revisions used a shared spin-trylock that dropped a whole batch whenever the reader held the lock; under a dense bulk dump this punched small holes into the SysEx byte stream and corrupted large objects such as Set List dumps — each lost byte offsetting the rest of the 8-to-7-encoded object.)*

### 8.3 Inbound - client MIDI -> Kronos

Write raw MIDI bytes to the TCP connection to inject them into the Kronos MIDI receive engine (`MidiInPortGeneric7Receive`). Any valid MIDI message type is accepted: note on/off, CC, program change, pitch bend, SysEx, real-time, etc.

Maximum single write: 4096 bytes (kernel module buffer limit). Larger payloads must be split across multiple writes.

### 8.4 Hub semantics

All connected clients share the same single input and output channel:

- Kronos MIDI output is broadcast to **all** connected clients simultaneously via `send(..., MSG_DONTWAIT)`. A blocked or slow client does not affect delivery to other clients (the send is non-blocking; data to a full-buffer client is dropped for that delivery). Each client socket is given a **256 KB send buffer** (`SO_SNDBUF`) so a momentary client stall cannot fill the buffer and drop a chunk in the middle of a large SysEx dump.
- MIDI injected by any client is delivered to the Kronos. Other clients do **not** receive an echo of injected data unless the Kronos itself echoes it back as MIDI output.

### 8.5 Ring cursor reset

The ring cursor is reset (pre-connection buffered data discarded) when the **first** client connects to an otherwise empty hub. Clients that connect while others are already active begin receiving from the current write position and may receive mid-message data if they arrive during an active capture burst. Clients should implement standard MIDI re-sync logic (scan forward for the next status byte with bit 7 set if the first byte received is a data byte).

### 8.6 Retrieving program & combi names (Kronos SysEx notes)

Pulling the names of every program/combi in a bank is a common tooling task, and the Kronos firmware has a **non-obvious asymmetry** that is easy to misdiagnose. These notes apply to any client (this daemon or otherwise) that requests Object Dumps over MIDI; they are properties of the **Kronos**, not of the bridge. All are hardware-verified (2026-07-08). Names are carried in Object Dump replies (`0x73`) at data offset 0 (24 bytes, 8→7 packed) for both the full object (`0x00` Program / `0x01` Combi) and the name-only object (`0x13` Program Name / `0x12` Combi Name).

**Bank numbers** (KRONOS MIDI Implementation, *2 bank map):

| Object | Preset / read-only banks | USER-writable banks |
|--------|--------------------------|---------------------|
| Program / Program Name (`0x00`/`0x13`) | `0x00`–`0x05` (INT-A…F), `0x10`–`0x1A` (GM, g(1)–g(9), g(d)) | `0x40`–`0x4D` (USER-A…G, AA…GG) |
| Combi / Combi Name (`0x01`/`0x12`) | `0x00`–`0x06` (INT-A…G) | `0x40`–`0x46` (USER-A…G) |

**The asymmetry — the whole-bank name enum is preset-only.** The **Dump Bank Request** (`func 0x77`, e.g. `F0 42 3g 68 77 13 <bank> F7`) works for the **preset** banks (INT, GM/g) and streams all 128 names in ~20 ms, but for every **USER-writable** bank it returns a **Reply (`func 0x24`) with code 4 "target object not found"** — even when the bank is fully populated. This is a firmware limitation of the name-only enum, not an empty bank and not a permissions issue.

- There is **no** "banks per session" throttle. A persistent belief that the Kronos rejects everything after ~13 banks was a misreading of *this* preset-only reject (preset banks dump, USER banks reject). Preset `0x77` enums and USER per-object fetches (below) both stream reliably with no session cap.

**The workaround — request each object individually with `func 0x72`.** The single **Object Dump Request** (`func 0x72`) works for **every** bank, including USER:

```
Request (one name):  F0 42 3g 68 72 <obj> <bank> <idH> <idL> F7
   obj  = 0x13 (Program Name) or 0x12 (Combi Name)
   idH  = (index >> 7) & 0x7F,  idL = index & 0x7F     (index 0..127)
Reply:               F0 42 3g 68 73 <obj> <bank> <idH> <idL> <version> <name...> F7
```

Loop `index` 0…127 to pull a whole USER bank. Empty slots reply with a **blank-named** object (not a reject), so all 128 indices respond for a bank that exists.

**Pacing is mandatory — never burst.** Injecting a bank's worth of requests back-to-back **overruns the Kronos MIDI-in**: it drops *every* reply and can corrupt a request in flight (a lost `F7`), which makes the Kronos pop a user-facing **"MIDI Receiving Error"** dialog. Two safe patterns, both verified at a clean 128/128:

- **Batched (recommended):** concatenate ~32 `0x72` requests into one injection (keep it under the ctrl port's ~2 KB line limit if using `MIDI_SEND`; the bridge's inbound path caps a single write at 4096 bytes — see §8.3), then **wait for that batch's `0x73` replies to go idle (~350 ms) before sending the next batch**. ~4 batches pull a 128-object bank in ~2 s.
- **Paced singles:** one `0x72` request every ~10 ms.

Also avoid **connection churn**: if you inject via the one-shot control port (`MIDI_SEND`, one TCP connection per command — §6.1), one-connection-per-request is ~2688 connections for a full 21-bank user sweep and can overwhelm the daemon. Batching (~4 injections/bank) or a persistent control session (§6.2) avoids this.

**Absent vs. empty vs. rejected** when driving a sweep: match replies by content (§7 `SYSEX` note) — a `0x73` with the requested `obj`/`bank` is a name (blank or not); a `0x24` code 4 with no `0x73` for that bank means the whole-bank enum was refused (you asked with `0x77` on a USER bank — switch to `0x72`). A bank that yields no reply to a full first batch of `0x72` requests (after a generous wait) is genuinely absent/nonexistent.

---

## 9. Button name reference

Button names are case-sensitive and must be uppercase.

### Navigation

| Name | Description |
|------|-------------|
| `EXIT` | Exit button |
| `ENTER` | Enter / confirm button |

### Mode select

| Name | Description |
|------|-------------|
| `SETLIST` | Setlist mode |
| `COMBI` | Combi mode |
| `PROGRAM` | Program mode |
| `SEQUENCE` | Sequence mode |
| `SAMPLING` | Sampling mode |
| `GLOBAL` | Global mode |
| `DISK` | Disk mode |

### Utility

| Name | Description |
|------|-------------|
| `HELP` | Help button |
| `COMPARE` | Compare button |
| `RESET` | Reset button |

### Numeric pad

| Name | Description |
|------|-------------|
| `NUM0` - `NUM9` | Numeric keys 0-9 |
| `NUM_DASH` | Numeric dash / minus |
| `NUM_DOT` | Numeric dot / decimal |

### Value control

| Name | Description |
|------|-------------|
| `INC` | Increment value |
| `DEC` | Decrement value |

### Mix Play buttons

`MP1` through `MP8`

### Mix Select buttons

`MS1` through `MS8`

### Bank buttons

| Name | Description |
|------|-------------|
| `BANK_IA` - `BANK_IG` | Internal banks A through G |
| `BANK_UA` - `BANK_UG` | User banks A through G |

### Sequencer

| Name | Description |
|------|-------------|
| `SEQ_START` | Sequencer start / stop |
| `SEQ_REC` | Sequencer record |
| `SEQ_LOCATE` | Locate / return to start |
| `SEQ_FF` | Fast forward |
| `SEQ_REW` | Rewind |
| `SEQ_PAUSE` | Pause |
| `TAP_TEMPO` | Tap tempo |

### Sampling

| Name | Description |
|------|-------------|
| `SMPL_REC` | Sampling record |
| `SMPL_START` | Sampling start |

### Channel strip

| Name | Description |
|------|-------------|
| `MIX_KNOBS` | Mix knobs selector |
| `SOLO` | Solo (fires on release; the daemon sends press+release so the release event triggers it) |

---

## 10. SYSINFO field reference

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

## 11. Authentication internals

Authentication is attempted in priority order. The first backend that recognises the username determines the result.

### 10.1 KronosNet.conf

File: `/korg/rw/Startup/KronosNet.conf`

Line 1: username (plain text)
Line 2: password (plain text)

This is the credential store managed by the Kronos UI (the network/FTP user). If the submitted username matches line 1, authentication either succeeds (passwords match) or fails (passwords differ) and no further backends are tried.

### 10.2 PublicID fallback

If `KronosNet.conf` is missing or does not contain the submitted username, the daemon accepts username `kronos` with the device's PublicID as the password. The PublicID is the dashed form shown in the Kronos UI (Global > Basic, Menu > Display Public ID), e.g. `AA-BB-CC-DD-EE-FF-00-11`. Dashes are optional - the daemon strips them before comparing, so both `AA-BB-CC-DD-EE-FF-00-11` and `AABBCCDDEEFF0011` are accepted. Any other username or password is rejected.

The PublicID is read from `/proc/id` (created by `GetPubIdMod.ko` at boot) and is unique per device. It is visible to the device owner but not guessable by an external attacker.

This fallback is intended as an emergency recovery path for screen connect only. It does not grant FTP access. It covers cases where `KronosNet.conf` is absent - for example on a Nautilus where the file may not exist or may be named differently. No directory flag or configuration is required to enable it.

### 10.3 Access log

Every authentication attempt (success or failure) is appended to `/korg/rw/screenremote/access.log` with a timestamp, client IP, and outcome:

```
2024-09-01 12:34:56  192.168.1.42   ACCEPTED
2024-09-01 12:35:01  192.168.1.99   REJECTED  user=unknown: user not found
```

---

## 12. Error handling and disconnection

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

## 13. Implementation limits

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
| midi_inject.ko open poll at startup | 2 seconds | 20 x 100 ms; MIDI disabled if /proc/.midi_in never appears |
| midi_tcp connect poll at startup | 6 seconds | 30 x 200 ms; SysEx capture unavailable if connection fails |
| MIDI_SEND max message size | 4096 bytes | Limited by the kernel module's static buffer |
| MIDI_SEND CC throttle interval | 7 ms | Minimum spacing between injected Control Change messages sharing the same (status, controller) |
| MIDI_SEND CC throttle tracked controllers | 32 | Concurrent (status, controller) pairs tracked; a 33rd distinct controller bypasses throttling |
| SysEx capture buffer (`SYSEX` command) | 65536 bytes | Max response for a single `SYSEX` command; larger objects must be collected off the MIDI bridge stream (§8), which has no cap |
| SysEx capture timeout | ~5 seconds | Initial 5 s recv timeout, then 1 s for trailing data |
| MIDI bridge max clients | 8 | Connections beyond this are rejected with no response |
| MIDI bridge SysEx stream chunk | 1024 bytes | SysEx is flushed to clients in ≤1 KB chunks; large objects (e.g. a ~79 KB Set List) stream with no total size cap |
| MIDI bridge output ring | 16384 bytes | Kernel lock-free single-producer/single-consumer ring; drops only on genuine overflow (unread data preserved); overflow byte count in `/proc/.midi_ports` as `ring_overflow_bytes` |
| MIDI bridge client send buffer | 262144 bytes | `SO_SNDBUF` per client socket, so a stalled client can't drop a chunk mid-dump |
| MIDI bridge inbound buffer | 4096 bytes | Per-write maximum for MIDI injection; split larger payloads across writes |
| Touch calibration: `touch_x_offset` | 10 | Pixels added to x before ADC scaling |
| Touch calibration: `touch_x_range` | 813 | Pixel span mapped to ADC 0-255 (horizontal) |
| Touch calibration: `touch_y_offset` | 20 | Pixels added to y before ADC scaling |
| Touch calibration: `touch_y_range` | 638 | Pixel span mapped to ADC 0-255 (vertical) |
