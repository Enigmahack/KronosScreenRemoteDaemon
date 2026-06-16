# KronosScreenRemoteDaemon

> **BETA SOFTWARE — USE AT YOUR OWN RISK**
>
> This software is provided in beta form and is **not production-ready**. It is distributed **as-is**, without any express or implied warranty of any kind, including but not limited to warranties of merchantability, fitness for a particular purpose, or non-infringement. The authors and contributors make no guarantees regarding stability, correctness, security, or suitability for any use case.
>
> **No support is offered or implied.** There is no commitment to fix bugs, respond to issues, or maintain compatibility with any hardware or software version. Use of this software is entirely at your own risk. The authors accept no liability for any damage to hardware, data loss, or any other consequence — direct or indirect — arising from the use or misuse of this software.
>
> By using this software you acknowledge that you understand and accept these terms.

A framebuffer streaming daemon and virtual keyboard kernel module for the **Korg Kronos** synthesizer. It streams the Kronos display (`/dev/fb1`, 8bpp 800x600) over TCP, and accepts remote control commands for touch, buttons, the data wheel, and keyboard input injection.

---

## Repository layout

```
source/
  screenremote.c          	- main daemon source (C99, i686, static)
  screenremote.cfg.example	- example config file
  Makefile                	- builds the screenremote binary
  palette_data.h          	- Kronos display palette table
  vkbd_ko.h               	- generated: embedded vkbd.ko as a C array (do not edit; not committed)

vkbd_module/
  vkbd.c                  	- virtual keyboard kernel module source
  Kbuild                  	- kernel build descriptor
  Makefile.module         	- builds vkbd.ko, patches it, then generates vkbd_ko.h
  vkbd.ko                 	- built by Makefile.module (not committed; generated)

tools/
  PackageMaker/
	payload/				- Your files, organised under the `mnt/` prefix (see below). Not committed — project-specific.
		mnt/
			korg/
				rw/
		md5sum/
			md5sum 	   		- Kronos executable required for validating package md5
	build_auto.py 			- auto-builds using default values
	build_package.py  		- Interactive package builder. Run this to produce a USB package.
	README.md				- README on how to use the PackageMaker
	runPackageBuilder.bat	- Batch file to run python script automatically
	sign_package.py		   	- Standalone signature generator. Re-sign or verify any package's scripts.

patch_init_offset.py      	- fixes struct module init offset mismatch (see below)
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
SS_TIMEOUT n           - set screensaver timeout in seconds (0 = disable)
STATE                  - reply: MODE=N\n  (0=init 1=Setlist 2=Combi 3=Program 4=Sequence 5=Sampling 6=Global 7=Disk)
VERSION                - reply: VER=x.x.x BUILD=xxx\n
SYSINFO                - reply: multi-line block of uptime, load, memory, CPU, audio, disk, USB, temperature, fan, mode
```

### UDP discovery

Any client on the LAN can send `"KSCR?"` (5 bytes) to UDP port 7372. The daemon replies with `"KSCR SP=<stream_port> CP=<ctrl_port>\n"`.

### Authentication

Credentials are validated in this order:

1. `/korg/rw/Startup/KronosNet.conf` - Korg UI-managed plaintext username/password
2. `/etc/shadow` then `/etc/passwd` - system users; supports `$1$` (MD5) and `$6$` (SHA-512) hashes (embedded implementation, no dependency on libcrypt)
3. vsftpd virtual-user Berkeley DB - plaintext password DB read directly without pam

### VGA mirror and screensaver

When `/korg/rw/screenremote/.mirror_enable` exists, the daemon opens `/dev/fb0` and continuously copies fb1 to it. A screensaver blanks fb0 after a configurable idle period (default 300 s); it wakes automatically when fb1 changes. The screensaver never affects fb1 (the Kronos display).

### Virtual keyboard

At startup the daemon extracts `vkbd.ko` from the binary (embedded as a C array in `vkbd_ko.h`) and loads it with `init_module(2)`. The module registers a virtual input device with the kernel input subsystem so Kronos's EVA process discovers it like a physical USB keyboard. Key injection is then done by writing `"code value\n"` to `/proc/.vkbd`. If the module is unavailable, injection falls back to `/dev/uinput`.

---

## Building

### Prerequisites

- `i686-linux-gnu-gcc` - cross-compiler targeting the Kronos CPU (32-bit x86)
- `make`
- `xxd` - used to convert `vkbd.ko` into the `vkbd_ko.h` C array

**To rebuild `vkbd.ko` from source** (not required if you use the pre-built module):

- Linux **2.6.32.11** kernel source tree, placed at `../linux-2.6.32.11` relative to the repo root (i.e. one level above this directory). The Kronos runs a patched 2.6.32.11 kernel; modules must be built against that exact tree to match the in-kernel ABI and symbol versions.
- **Python 3** - required to run `patch_init_offset.py` during the module build (see below).

### Step 1 - build the kernel module (optional)

```sh
make -C vkbd_module -f Makefile.module
```

This runs `make modules` against the 2.6.32.11 kernel tree, runs `patch_init_offset.py` on the resulting `vkbd.ko`, and then uses `xxd -i` to regenerate `source/vkbd_ko.h`.

Step 1 is required — `vkbd.ko` and `vkbd_ko.h` are not committed and must be built before Step 2.

### Step 2 - build the daemon

```sh
make -C source
```

Output: `build/screenremote` - a statically linked i686 ELF binary.

---

## patch_init_offset.py

The Kronos kernel's `struct module` places the `init` function pointer at offset `0xd4`. Vanilla Linux 2.6.32 headers and GCC 13 place it at `0xbc`. If the module is loaded unpatched, the kernel writes the `init_module` address to the wrong offset and the module's init function is never called.

`patch_init_offset.py` fixes this by editing the ELF relocation table of `vkbd.ko` - it finds the `.rel.gnu.linkonce.this_module` section, locates the `init_module` relocation entry, and changes its `r_offset` from `0xbc` to `0xd4`. This is done automatically by `Makefile.module` immediately after the kernel build step.

**Requirements:** Python 3 (standard library only - `struct`, `sys`, `os`).

**Manual use:**

```sh
python3 patch_init_offset.py vkbd_module/vkbd.ko
```

---

## Configuration

Copy `source/screenremote.cfg.example` to `/korg/rw/screenremote/screenremote.cfg` on the Kronos and edit as needed. All keys are optional; defaults are shown below.

```ini
#stream_port=7373
#ctrl_port=7374
#screensaver_timeout=300
```

`screensaver_timeout` is in seconds; set to `0` to disable. The `/korg/rw/screenremote/` directory is created automatically by the daemon on first run.

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
status[1] = 0x01 (auth fail) or 0x02 (service unavailable)
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

**Embedded binary blob.** The daemon contains `vkbd.ko` compiled into it as a raw byte array (`vkbd_ko.h`) and extracts and loads it at runtime using the `init_module(2)` syscall directly, without shelling out to `insmod`. Self-extracting executables that carry and load additional binaries are a well-known malware distribution technique; heuristic scanners flag the pattern regardless of intent. The reason for doing this here is that `system()` and `/bin/sh` are unavailable on non-rooted Kronos units.

**Direct kernel module loading.** Calling `init_module(2)` to load a kernel module without going through `/sbin/insmod` is unusual enough to be a trigger on its own. Rootkits use the same syscall to load modules that hide processes or intercept system calls. The module here only registers a virtual keyboard with the input subsystem.

**Reads `/etc/shadow` and `/etc/passwd`.** Credential harvesting malware and password crackers read these files. The daemon reads them to authenticate the connecting client against the Kronos system user database. A better way was found, and this will likely be removed in future releases. 

**Embedded cryptographic implementations.** The daemon includes self-contained MD5 and SHA-512 implementations (to avoid a runtime dependency on libcrypt, which was removed from glibc's static archive in glibc 2.28). Rolling your own crypto is a flag because malware sometimes embeds encryption routines. The algorithms here are only used for password hash verification. Also may be removed on future releases. 

**Input event injection.** Writing to `/dev/uinput` and `/dev/rtf5` to inject keystrokes and touchscreen events is a behavioral signature of keyloggers and Remote Access Tools. This is exactly the intended use here - remote control of the Kronos UI from a client application.

**Statically linked binary.** The binary has no shared library dependencies. Statically linking is common for embedded targets (the Kronos has a minimal filesystem), but it is also a common malware technique for portability and to avoid detection via library monitoring.

**Screen capture over TCP.** The core function of the daemon - reading the framebuffer and streaming it over an authenticated TCP connection - matches the behavioral profile of a Remote Access Tool or spyware at the network level. It is, intentionally, a remote screen viewer.

Taken individually each of these patterns has a legitimate explanation. Taken together on a single statically linked binary they push most heuristic classifiers well past their detection threshold. If you need to share the binary through a service that blocks it, archiving it in a password-protected zip (password noted in accompanying text) is usually sufficient to pass it through, since the scanner cannot inspect the contents.
