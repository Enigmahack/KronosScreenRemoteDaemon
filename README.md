# KronosScreenRemoteDaemon

> **USE AT YOUR OWN RISK**

> This software is not affiliated with Korg in any way, and while it may be stable with tested versions of the Korg Kronos OS (3.2.0 - 3.2.2), it may add additional risk to the stability of your Korg Kronos. 
> It is distributed **as-is**, without any express or implied warranty of any kind, including but not limited to warranties of merchantability, fitness for a particular purpose, or non-infringement. The authors and contributors make no guarantees regarding stability, correctness, security, or suitability for any use case.
>
> **No support is offered or implied.** There is no commitment to fix bugs, respond to issues, or maintain compatibility with any hardware or software version. Use of this software is entirely at your own risk. The authors accept no liability for any damage to hardware, data loss, or any other consequence — direct or indirect — arising from the use or misuse of this software.
>
> By using this software you acknowledge that you understand and accept these terms.


Kronos Screen Remote Daemon: A framebuffer streaming daemon with virtual keyboard and MIDI injection kernel modules for the **Korg Kronos** synthesizer. It streams the Kronos display (`/dev/fb1`, 8bpp 800x600) over TCP, accepts remote control commands for touch, buttons, the data wheel, and keyboard input injection, and provides MIDI injection with SysEx request/response support.

---

## Repository layout

```
source/
  screenremote.c            - main daemon source (C99, i686, static)
  midi_tcp.c                - TCP MIDI bridge subprocess (injected into screenremote as embedded binary)
  screenremote.cfg.example  - example config file
  Makefile                  - builds the screenremote binary (including midi_inject_ko.h and midi_tcp_bin.h)
  palette_data.h            - Kronos display palette table
  no_tracepoints.h          - stub header for kernel tracepoint macros
  vkbd_ko.h                 - generated: embedded vkbd.ko as a C array (do not edit; not committed)
  midi_inject_ko.h          - generated: embedded midi_inject.ko as a C array (do not edit; not committed)
  midi_tcp_bin.h             - generated: embedded midi_tcp binary as a C array (do not edit; not committed)

vkbd_module/
  vkbd.c                    - virtual keyboard kernel module source
  Kbuild                    - kernel build descriptor
  Makefile.module            - builds vkbd.ko, patches it, then generates vkbd_ko.h
  vkbd.ko                   - built by Makefile.module (not committed; generated)

midi_module/
  midi_inject.c             - MIDI injection kernel module source (/proc/.midi_in write, /proc/.midi_ring read)
  Kbuild                    - kernel build descriptor
  Makefile.module            - builds midi_inject.ko, patches it, then generates midi_inject_ko.h

tools/
  PackageMaker/
    payload/                 - Build dependencies and user payload files
      DisplayUpdaterMessage/ - Kronos updater UI binary (required by package builder)
      md5sum/                - i386 Linux md5sum binary (required for package integrity checks)
      mnt/korg/rw/           - your payload files, maps 1:1 to Kronos filesystem (not committed)
    build_package.py         - interactive package builder (main entry point)
    build_auto.py            - non-interactive one-shot builder for ScreenRemote
    build_cleaner.py         - Factory State Restore builder (also standalone)
    build_unroot.py          - Root Cleaner (unroot) builder (also standalone)
    sign_package.py          - standalone signature generator / verifier
    runPackageBuilder.bat    - Windows launcher for build_package.py
    README.md                - detailed PackageMaker documentation

docs/
  api.md                     - full API and wire protocol reference

patch_init_offset.py         - fixes struct module init offset mismatch (see below)
```

---

## How it works

### Streaming

The daemon opens `/dev/fb1` (read-only mmap) and listens on three sockets:

| Socket | Default port | Purpose |
|--------|-------------|---------|
| TCP | 7373 | Framebuffer stream |
| TCP | 7374 | Control commands |
| UDP | 7372 | LAN discovery (fixed, never configurable) |
| TCP | 9875 | MIDI bridge (localhost only; internal use between daemon and midi_tcp subprocess) |

A client connects to the stream port and completes a handshake. On success the server sends the display resolution and 256-entry RGB palette, then begins streaming frames.

Two streaming modes are supported:

- **Pull** (`0x01`) - client sends `0xFF` to request each frame; server sends the full framebuffer immediately.
- **Change** (`0x02`) - server sends frames at the negotiated FPS rate but only when the framebuffer content has changed. Unchanged frames are skipped. Changed regions are compressed with PackBits RLE and sent as dirty-rect updates; full frames are sent only when the RLE output would exceed a full frame in size.

### Control port

Control connections are accepted only from the IP address of the currently authenticated stream client. A control connection sends one newline-terminated command and receives a response, or it can open a persistent session with `CTRL_PERSIST`.

Supported commands:

```
CTRL_PERSIST           - keep the connection open for further commands
MIRROR_ON / MIRROR_OFF - enable / disable VGA output mirror (fb0)
TOUCH nx ny            - touchscreen tap (press + release) at pixel coords
TOUCH_DOWN nx ny       - pen-down only
TOUCH_MOVE nx ny       - pen-move
TOUCH_UP nx ny         - pen-up only
BUTTON name            - press + release a named front-panel button
CHORD name1 name2      - chord: press name1, press name2, release name2, release name1
WHEEL CW|CCW           - one data-wheel tick
KEY code val           - raw key inject: code 1-511, val 0=release 1=press
REFRESH                - force full-frame resend on next tick
MIDI_SEND hex          - inject raw MIDI bytes (hex pairs, spaces allowed)
SYSEX hex              - send SysEx (must start F0), capture response (up to 5 s)
                         reply: SYSEX_RESP hex\n  or  ERR TIMEOUT\n
MIDI_STATUS            - reply: MIDI_LOADED=n\nMIDI_IN=n\nMIDI_CAPTURE=n\nOK\n
SS_TIMEOUT n           - set screensaver timeout in seconds (0 = disable)
STATE                  - reply: MODE=N\n  (0=init 1=Setlist 2=Combi 3=Program 4=Sequence 5=Sampling 6=Global 7=Disk)
VERSION                - reply: VER=x.x.x BUILD=xxx\n
SYSINFO                - reply: multi-line block of uptime, load, memory, CPU, audio, disk, USB, temperature, fan, mode
```

### UDP discovery

Any client on the LAN can send `"KSCR?"` (5 bytes) to UDP port 7372. The daemon replies with `"KSCR SP=<stream_port> CP=<ctrl_port> MIDI=<0|1>\n"` where `MIDI=1` indicates the MIDI injection module loaded successfully.

### Authentication

Credentials are validated in this order:

1. `/korg/rw/Startup/KronosNet.conf` - Korg UI-managed plaintext username/password
2. **PublicID fallback** - if `KronosNet.conf` does not recognise the user, `kronos` + the device's PublicID is always accepted as a password (the dashed form from Global > Basic, Menu > Display Public ID, e.g. `AA-BB-CC-DD-EE-FF-00-11`; dashes are optional). This is an emergency recovery path for screen connect only (not FTP), covering cases where `KronosNet.conf` is missing or the device is a Nautilus variant that does not have the file. No directory flag is required.

### VGA mirror and screensaver

When `/korg/rw/screenremote/.mirror_enable` exists, the daemon opens `/dev/fb0` and continuously copies fb1 to it. A screensaver blanks fb0 after a configurable idle period (default 300 s); it wakes automatically when fb1 changes. The screensaver never affects fb1 (the Kronos display).

### Virtual keyboard

At startup the daemon extracts `vkbd.ko` from the binary (embedded as a C array in `vkbd_ko.h`) and loads it with `init_module(2)`. The module registers a virtual input device with the kernel input subsystem so Kronos's EVA process discovers it like a physical USB keyboard. Key injection is then done by writing `"code value\n"` to `/proc/.vkbd`. If the module is unavailable, injection falls back to `/dev/uinput`.

### MIDI injection

At startup the daemon also loads `midi_inject.ko` (embedded as `midi_inject_ko.h`) with `init_module(2)`, passing kernel symbol addresses for `MidiInPortGeneric7Receive` and `RegisterMidiInPort` resolved from `/proc/kallsyms`. The module creates two proc entries:

- `/proc/.midi_in` — write raw MIDI bytes to inject into the Kronos MIDI engine (OA.ko)
- `/proc/.midi_ring` — read SysEx responses from the hardware ring buffer (Block 5) at kernel speed

The daemon then spawns an embedded `midi_tcp` subprocess (built from `source/midi_tcp.c`, embedded as `midi_tcp_bin.h`) that listens on TCP port 9875 (localhost only). This subprocess is a single-threaded MIDI bridge: it writes inbound TCP bytes directly to `/proc/.midi_in` and reads MIDI output from `/proc/.midi_ring` (or falls back to shared memory), parsing the byte stream into complete MIDI messages and forwarding each one to TCP as it arrives. All message types — note on/off, CC, SysEx, real-time — are forwarded in the order they are produced by the Kronos, without buffering or request/response pairing. The daemon connects to the subprocess on localhost and uses it to handle `MIDI_SEND`, `SYSEX`, and `MIDI_STATUS` control commands. Because MIDI output is delivered asynchronously and unfiltered, SysEx responses may arrive interleaved with other MIDI messages; clients match responses to requests by inspecting the SysEx payload (Korg manufacturer ID 0x42, model ID 0x58, and function code) rather than by timing or stream position.

If the kernel symbols cannot be resolved (e.g. the Kronos OS version is too old or the symbols are stripped), MIDI injection is silently disabled and `MIDI_STATUS` will report `MIDI_LOADED=0`.

---

## Building

### Prerequisites

- `i686-linux-gnu-gcc` - cross-compiler targeting the Kronos CPU (32-bit x86)
- `make`
- `xxd` - used to convert `vkbd.ko` into the `vkbd_ko.h` C array

**To rebuild `vkbd.ko` from source** (not required if you use the pre-built module):

- Linux **2.6.32.11** kernel source tree, placed at `../linux-2.6.32.11` relative to the repo root (i.e. one level above this directory). The Kronos runs a patched 2.6.32.11 kernel; modules must be built against that exact tree to match the in-kernel ABI and symbol versions.
- **Python 3** - required to run `patch_init_offset.py` during the module build (see below).

### Step 1 - build the kernel modules

```sh
make -C vkbd_module -f Makefile.module
make -C midi_module -f Makefile.module
```

Each runs `make modules` against the 2.6.32.11 kernel tree, runs `patch_init_offset.py` on the resulting `.ko`, and then uses `xxd -i` to regenerate the corresponding C header (`source/vkbd_ko.h`, `source/midi_inject_ko.h`).

Step 1 is required — the `.ko` files and their generated headers are not committed and must be built before Step 2.

### Step 2 - build the daemon

```sh
make -C source
```

Output: `build/screenremote` - a statically linked i686 ELF binary.

---

## patch_init_offset.py

The Kronos kernel's `struct module` places the `init` function pointer at offset `0xd4`. Vanilla Linux 2.6.32 headers and GCC 13 place it at `0xbc`. If the module is loaded unpatched, the kernel writes the `init_module` address to the wrong offset and the module's init function is never called.

`patch_init_offset.py` fixes this by editing the ELF relocation table of the `.ko` file - it finds the `.rel.gnu.linkonce.this_module` section, locates the `init_module` relocation entry, and changes its `r_offset` from `0xbc` to `0xd4`. This is done automatically by each module's `Makefile.module` immediately after the kernel build step. It applies to both `vkbd.ko` and `midi_inject.ko`.

**Requirements:** Python 3 (standard library only - `struct`, `sys`, `os`).

**Manual use:**

```sh
python3 patch_init_offset.py vkbd_module/vkbd.ko
python3 patch_init_offset.py midi_module/midi_inject.ko
```

---

## MIDI injection history

An earlier approach (`vusb_midi/`, since removed) attempted to use `dummy_hcd` to inject MIDI messages via a virtual USB gadget interface. This was abandoned because `dummy_hcd` is incompatible with the Kronos's RTAI real-time kernel — the URB completion tasklets conflict with RTAI scheduling and cause kernel panics. The current approach calls the Kronos MIDI receive function directly from a kernel module (`midi_module/midi_inject.c`) and bridges SysEx request/response via an embedded TCP subprocess.

---

## Configuration

Copy `source/screenremote.cfg.example` to `/korg/rw/screenremote/screenremote.cfg` on the Kronos and edit as needed. All keys are optional; defaults are shown below.

```ini
#stream_port=7373
#ctrl_port=7374
#screensaver_timeout=300
#touch_x_offset=10
#touch_x_range=813
#touch_y_offset=20
#touch_y_range=638
```

`screensaver_timeout` is in seconds; set to `0` to disable. The `touch_*` keys adjust the pixel-to-ADC mapping for touchscreen injection; the defaults are correct for standard Kronos units but can be tweaked if touch events land at the wrong position. The `/korg/rw/screenremote/` directory is created automatically by the daemon on first run.

---

## Deployment

On a rooted Kronos, files can be SCP'd directly to `/korg/rw/screenremote`, however the recommended path is still to generate a package using the `Package Maker` under the `tools` directory, and install via USB + Kronos installer. 

Review the Package Maker readme for more detailed instructions.

An access log is written to `/korg/rw/screenremote/access.log`.

---

## Protocol wire format

### Handshake - client to server

```
MAGIC[4]  = "KSCR"
ver[1]    = 0x02
mode[1]   = 0x01 (Pull) or 0x02 (Change)
fps[1]    = requested frame rate (1-15; 0 = use maximum)
ulen[1]   = username length
plen[1]   = password length
username[ulen]
password[plen]
```

### Handshake - server to client (success)

```
MAGIC[4]  = "KSCR"
status[1] = 0x00
width[2]  = framebuffer width, little-endian
height[2] = framebuffer height, little-endian
palette[256*3] = RGB8 palette entries
```

### Handshake - server to client (failure)

```
MAGIC[4]  = "KSCR"
status[1] = 0x01 (auth fail) or 0x02 (user not found)
```

### Full frame

```
len[4]          = frame_bytes (width * height), little-endian
pixels[len]     = raw 8bpp pixel data, row-major
```

### Dirty-rect update (Change mode only; len < frame_bytes)

```
len[4]          = payload length = 4 + rle_bytes, little-endian
first_row[2]    = first changed row, little-endian
row_count[2]    = number of changed rows, little-endian
rle[...]        = PackBits-encoded pixel data for the changed rows
```

A client can distinguish full frames from dirty-rect updates by comparing `len` to `width * height`; dirty-rect payloads are always strictly smaller.

---

## Documentation

Full API reference: [`docs/api.md`](docs/api.md)

Covers every wire format, all control commands and their arguments, the button name table, SYSINFO fields, authentication internals, error handling, and implementation limits.

---

## Antivirus false positives

Some online file-scanning services and cloud storage providers (including Google Drive) may flag the `screenremote` binary or `vkbd.ko` as suspicious or malicious. These are **false positives**. Nothing in this software is harmful; the detections are purely heuristic and stem from several legitimate implementation choices that happen to match patterns that generic scanners look for:

**Embedded binary blobs.** The daemon contains three binaries compiled into it as raw byte arrays (`vkbd_ko.h`, `midi_inject_ko.h`, `midi_tcp_bin.h`) and extracts and loads them at runtime — the kernel modules via the `init_module(2)` syscall directly, and the MIDI bridge via `fork()`/`execl()`. Self-extracting executables that carry and load additional binaries are a well-known malware distribution technique; heuristic scanners flag the pattern regardless of intent. The reason for doing this here is that `system()` and `/bin/sh` are unavailable on non-rooted Kronos units.

**Direct kernel module loading.** Calling `init_module(2)` to load kernel modules without going through `/sbin/insmod` is unusual enough to be a trigger on its own. Rootkits use the same syscall to load modules that hide processes or intercept system calls. The modules here only register a virtual keyboard with the input subsystem and provide a MIDI injection interface.

**Input event injection.** Writing to `/dev/uinput` and `/dev/rtf5` to inject keystrokes and touchscreen events is a behavioral signature of keyloggers and Remote Access Tools. This is exactly the intended use here - remote control of the Kronos UI from a client application.

**Statically linked binary.** The binary has no shared library dependencies. Statically linking is common for embedded targets (the Kronos has a minimal filesystem), but it is also a common malware technique for portability and to avoid detection via library monitoring.

**Screen capture over TCP.** The core function of the daemon - reading the framebuffer and streaming it over an authenticated TCP connection - matches the behavioral profile of a Remote Access Tool or spyware at the network level. It is, intentionally, a remote screen viewer.

Taken individually each of these patterns has a legitimate explanation. Taken together on a single statically linked binary they push most heuristic classifiers well past their detection threshold. If you need to share the binary through a service that blocks it, archiving it in a password-protected zip (password noted in accompanying text) is usually sufficient to pass it through, since the scanner cannot inspect the contents.
