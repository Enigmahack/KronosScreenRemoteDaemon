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
8. [Button name reference](#8-button-name-reference)
9. [SYSINFO field reference](#9-sysinfo-field-reference)
10. [Authentication internals](#10-authentication-internals)
11. [Error handling and disconnection](#11-error-handling-and-disconnection)
12. [Implementation limits](#12-implementation-limits)

---

## 1. Ports and addressing

| Transport | Default port | Configurable | Purpose |
|-----------|-------------|--------------|---------|
| TCP | 7373 | Yes (`stream_port` in config) | Framebuffer stream |
| TCP | 7374 | Yes (`ctrl_port` in config) | Remote control |
| UDP | 7372 | No | LAN discovery |

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
KSCR SP=<stream_port> CP=<ctrl_port>\n
```

Example:

```
KSCR SP=7373 CP=7374\n
```

Both port numbers are decimal ASCII. There is no authentication on discovery; the daemon always responds.

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
| `0x02` | Service unavailable (authentication lookup error - user not found in any backend) |

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
- If no stream client has ever connected (since the last daemon start), control connections are accepted from any IP.
- A control connection from a disallowed IP is immediately closed with no response.

This means a client must authenticate on the stream port before it can use the control port.

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

ADC values are computed from pixel coordinates:

```
h_adc = 10 + (x * 236) / 799    (horizontal, range ~10-246)
v_adc = 8  + (y * 237) / 599    (vertical,   range ~8-245)
```

---

### BUTTON

Press and release a named front-panel button. See [Section 8](#8-button-name-reference) for the full list of button names.

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

Press two buttons as a chord: press the first, press the second, release the second, release the first.

```
Request:  CHORD <name1> <name2>\n
Response: OK\n  (both buttons found)
          ERR\n (either button name not recognised)
```

| Argument | Type | Description |
|----------|------|-------------|
| name1 | string | First button (held down first, released last) |
| name2 | string | Second button (pressed and released while name1 is held) |

Four `/dev/rtf5` packets are sent in sequence.

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

Set the position of a CC slider or RT knob (1–8).

```
Request:  SLIDER <n> <value>\n
Response: OK\n
          ERR\n (n not 1-8, value not 0-127, or parse failure)
```

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| n | integer | 1–8 | Controller index (1 = leftmost slider/knob) |
| value | integer | 0–127 | Absolute position |

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
| value | integer | 0–127 | Absolute slider position |

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
VER=1.5.4 BUILD=20240901-1.5.4\n
```

`BUILD` is set at compile time from the date and version string.

---

### SYSINFO

Query a snapshot of system metrics. The response is a multi-line block terminated by `OK\n`.

```
Request:  SYSINFO\n
Response: <key>=<value>\n ... OK\n
```

See [Section 9](#9-sysinfo-field-reference) for the full field reference.

Note: CPU percentage fields require two successive `SYSINFO` calls to be meaningful. The first call will report `-1` for all `CPU*_PCT` fields because there is no prior sample to compute a delta from. The second and subsequent calls report the average CPU utilisation since the previous call.

---

## 8. Button name reference

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

## 9. SYSINFO field reference

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

## 10. Authentication internals

Authentication is attempted in priority order. The first backend that recognises the username determines the result.
10.2 and 10.3 are failsafes, but in virtually every known use case are not used and will likely be removed in future releases. 

### 10.1 KronosNet.conf

File: `/korg/rw/Startup/KronosNet.conf`

Line 1: username (plain text)
Line 2: password (plain text)

This is the credential store managed by the Kronos UI (the network/FTP user). If the submitted username matches line 1, authentication either succeeds (passwords match) or fails (passwords differ) and no further backends are tried.

### 10.2 /etc/shadow and /etc/passwd

The daemon reads `/etc/shadow` first; if the user is not found it falls back to `/etc/passwd`. It looks for a line beginning with `<username>:` and extracts the hash field.

Supported hash formats:

| Prefix | Algorithm |
|--------|-----------|
| `$1$` | MD5-crypt (Drepper, 1000 rounds) |
| `$6$` | SHA-512-crypt (Drepper, default 5000 rounds, configurable) |

Both algorithms are implemented inline in the daemon with no dependency on libcrypt or any external library.

Accounts with a hash field of `!` or `*` (locked accounts) are rejected immediately without trying further backends.

### 10.3 vsftpd Berkeley DB fallback

The daemon parses `/etc/pam.d/vsftpd` to extract the `db=` path from the `pam_userdb.so` line, appends `.db`, and opens the file directly as a Berkeley DB 4.x hash database (magic `0x00061561`). Passwords in this database are stored in plaintext.

If the PAM file cannot be parsed, the daemon tries a set of well-known fallback paths:

- `/etc/vsftpd/login.db`
- `/etc/vsftpd/virtual_users.db`
- `/etc/vsftpd/vsftpd_login.db`

### 10.4 Access log

Every authentication attempt (success or failure) is appended to `/korg/rw/screenremote/access.log` with a timestamp, client IP, and outcome:

```
2024-09-01 12:34:56  192.168.1.42   ACCEPTED
2024-09-01 12:35:01  192.168.1.99   REJECTED  user=unknown: user not found
```

---

## 11. Error handling and disconnection

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

## 12. Implementation limits

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
