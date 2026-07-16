# KronosScreenRemoteDaemon

> **USE AT YOUR OWN RISK**

> This software is not affiliated with Korg in any way, and while we have validated it functions correctly with OS 3.2, and some testing has been done with 3.1 and 3.0, we cannot 100% guarantee it will function in every circumstance, though it should. 
>
> It is distributed **as-is**, without any express or implied warranty of any kind, including but not limited to warranties of merchantability, fitness for a particular purpose, or non-infringement. The authors and contributors make no guarantees regarding stability, correctness, security, or suitability for any use case.
>
> **No support is offered or implied.** There is no commitment to fix bugs, respond to issues, or maintain compatibility with any hardware or software version. Use of this software is entirely at your own risk. The authors accept no liability for any damage to hardware, data loss, or any other consequence - direct or indirect - arising from the use or misuse of this software.
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
  Makefile                  - builds the screenremote binary (including the generated *_ko.h / *_bin.h headers below)
  palette_data.h            - Kronos display palette table
  no_tracepoints.h          - stub header for kernel tracepoint macros
  vkbd_ko.h                 - generated: embedded vkbd.ko as a C array (do not edit; not committed)
  midi_bridge_ko.h          - generated: embedded midi_bridge.ko as a C array (do not edit; not committed)
  nks4_inject_ko.h          - generated: embedded nks4_inject.ko as a C array (do not edit; not committed)
  midi_tcp_bin.h             - generated: embedded midi_tcp binary as a C array (do not edit; not committed)

vkbd_module/
  vkbd.c                    - virtual keyboard kernel module source
  Kbuild                    - kernel build descriptor
  Makefile.module            - builds vkbd.ko, patches it, then generates vkbd_ko.h
  vkbd.ko                   - built by Makefile.module (not committed; generated)

midi_module/
  midi_bridge.c             - MIDI injection kernel module source (/proc/.midi_in write, /proc/.midi_ring read)
  Kbuild                    - kernel build descriptor
  Makefile.module            - builds midi_bridge.ko, patches it, then generates midi_bridge_ko.h
  archive/                  - superseded midi_inject.c (predecessor of midi_bridge.c) kept for reference only; not built

nks4_inject_module/
  nks4_inject.c              - front-panel injection kernel module source (/proc/.nks4inject write, /proc/.nks4inject_status read)
  Kbuild                    - kernel build descriptor
  Makefile.module            - builds nks4_inject.ko, patches it, then generates nks4_inject_ko.h

tools/
  kscr_client.py             - Python 3 reference client (stream, control, and MIDI-bridge protocols)
  mock_kscr_server.py         - host-side mock KSCR server for testing kscr_client.py without hardware
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
  nks4_firmware_crossref.md  - cross-checks against the panel controller's own firmware (kronosology VSB RE)

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
| TCP | 9875 | MIDI bridge (`INADDR_ANY`, reachable from any host on the LAN; hub, up to 8 clients, no auth) — raw MIDI in/out incl. SysEx dumps; see api.md §8 |

A client connects to the stream port and completes a handshake. On success the server sends the display resolution and 256-entry RGB palette, then begins streaming frames.

Two streaming modes are supported:

- **Pull** (`0x01`) - client sends `0xFF` to request each frame; server sends the full framebuffer immediately.
- **Change** (`0x02`) - server sends frames at the negotiated FPS rate but only when the framebuffer content has changed. Unchanged frames are skipped. Changed regions are compressed with PackBits RLE and sent as dirty-rect updates; full frames are sent only when the RLE output would exceed a full frame in size.

### Control port

Control connections are accepted only from the IP address of the currently authenticated stream client, with one exception: a short read-only allowlist (`LASTTOUCH`, `PADMAP_LIST`, `PADMAP_STATE`, `PIXEL`, `REGION`, `PALETTE`, `STATE`, `VERSION`, `SYSINFO`) is answered from any IP. A control connection sends one newline-terminated command and receives a response, or it can open a persistent session with `CTRL_PERSIST` - see api.md §5.2/§6.2/§13 for full access-control, ownership-revalidation, and timeout details.

Supported commands:

```
CTRL_PERSIST           - keep the connection open for further commands
MIRROR_ON / MIRROR_OFF - enable / disable VGA output mirror (fb0)
TOUCH nx ny            - touchscreen tap (press + release) at pixel coords
TOUCH_DOWN nx ny       - pen-down only
TOUCH_MOVE nx ny       - pen-move
TOUCH_UP nx ny         - pen-up only
PADCHORD pad velocity  - play/release one of the 8 "Pads (touch to play)" chords by index (0-7), bypassing touch
PADMAP pad x0 y0 x1 y1 - live-set a pad's rectangular hit box (framebuffer pixel space)
PADMAP_LIST            - list all 8 configured pad hit boxes
PADMAP_ON / PADMAP_OFF - enable/disable auto-bridging real Pads-page taps to PADCHORD
LASTTOUCH               - reply: X=x Y=y\n  (most recent touch's raw pixel coords; calibration aid)
PADMAP_STATE            - diagnostic dump of PADMAP gating state (page/mode detection, last-gate results)
BUTTON name            - press + release a named front-panel button
CHORD [ms] n1 n2 [..n8] - press n1..nN left-to-right, hold ms (default 0), release right-to-left
WHEEL CW|CCW           - one data-wheel tick
SLIDER n value         - set physical Slider n (1-8) to value (0-127)
KNOB n value           - set physical RT Knob n (1-8) to value (0-127)
VSLIDER value          - set the value slider position (0-127)
JOYSTICK X|Y value     - set Joystick axis (0-127)
VECTOR X|Y value       - set Vector Joystick axis (0-127)
RIBBON X|Z value       - set Ribbon controller axis (0-127); Z axis unverified
AFTERTOUCH value       - set keybed channel aftertouch (0-127)
PEDAL value            - set assignable rear-panel PEDAL jack (0-127)
FOOTSWITCH value       - set assignable rear-panel foot SWITCH jack (0-127)
DAMPER value           - set DAMPER jack (0-127), ramped internally
TEMPO value            - set tempo (0-127, maps to ~40-300bpm non-linearly), ramped internally
KEY code val           - raw key inject: code 1-511, val 0=release 1=press
REFRESH                - force full-frame resend on next tick
PALETTE                 - reply: 256-entry RGB hex dump of the current palette (calibration aid)
REGION x0 y0 x1 y1      - reply: hex dump of a fb1 pixel rectangle, up to 8192 pixels (calibration aid)
PIXEL x y               - reply: V=n\n  (raw fb1 palette index at a pixel; calibration aid)
MIDI_SEND hex          - inject raw MIDI bytes (hex pairs, spaces allowed)
SYSEX hex              - send SysEx (must start F0), capture response (up to 5 s)
                         reply: SYSEX_RESP hex\n  or  ERR TIMEOUT\n
MIDI_STATUS            - reply: MIDI_LOADED=n\nMIDI_IN=n\nMIDI_CAPTURE=n\nOK\n
SS_TIMEOUT n           - set screensaver timeout in seconds (0 = disable)
STATE                  - reply: MODE=N\n  (0=init 1=Setlist 2=Combi 3=Program 4=Sequence 5=Sampling 6=Global 7=Disk)
VERSION                - reply: VER=x.x.x BUILD=xxx\n
SYSINFO                - reply: multi-line block of uptime, load, memory, CPU, audio, disk, USB, temperature, fan, mode
```

Front-panel commands (`TOUCH*`, `BUTTON`, `CHORD`, `WHEEL`, `SLIDER`, `KNOB`, `VSLIDER`, `JOYSTICK`, `VECTOR`, `RIBBON`, `AFTERTOUCH`, `PEDAL`, `FOOTSWITCH`, `DAMPER`, `TEMPO`, `PADCHORD`, `PADMAP`) return `ERR NKS4_NOT_LOADED\n` if `nks4_inject.ko` failed to load. See api.md §7 for full argument ranges, snapping/rejection rules, and internal encodings.

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

### Front-panel injection

`TOUCH*`, `BUTTON`, `CHORD`, `WHEEL`, `SLIDER`, `KNOB`, `VSLIDER`, `JOYSTICK`, `VECTOR`, `RIBBON`, `AFTERTOUCH`, `PEDAL`, `FOOTSWITCH`, `DAMPER`, `TEMPO`, and `PADCHORD` are injected through `nks4_inject.ko` (extracted from `nks4_inject_ko.h` and loaded with `init_module(2)` early at startup, right after `vkbd.ko`, well before EVA has drawn its UI — unlike `midi_bridge.ko` below, it does not need to wait). The module exposes `/proc/.nks4inject` (write) and `/proc/.nks4inject_status` (read) and calls OA's real `CSTGFrontPanel::HandleSwitchEvent` / `HandleTouchPanel` / `HandleRotary` / `HandleAnalogController` directly — the exact functions a physical press/touch/turn dispatches through — so injected events get bit-for-bit the same response as hardware, independent of whatever mode Eva is currently in. This replaced an earlier approach that wrote synthetic packets to `/dev/rtf5` (OA's own *outbound* notification FIFO to Eva, not an input path), which only ever fooled Eva's UI-mirroring logic and never reached transport, tempo, or KARMA control-surface state.

If `nks4_inject.ko` fails to load (symbol resolution failure against `/proc/kallsyms`, the boot-safety kill-switch present, OA not yet at the Live module state, etc.), every command in this list returns `ERR NKS4_NOT_LOADED\n` rather than silently falling back to the old `/dev/rtf5` path.

`PADCHORD` (and the `PADMAP`/`PADMAP_LIST`/`PADMAP_ON`/`PADMAP_OFF`/`LASTTOUCH`/`PADMAP_STATE` bridge built on top of it) plays or releases one of the 8 "Pads (touch to play)" chords directly via OA's `RT_chord_trigger`, bypassing Eva/touch entirely — the on-screen Pads grid doesn't dispatch through `HandleTouchPanel` the way every other touch widget does, so it needed its own path. With `PADMAP_ON`, real taps on the calibrated on-screen pad regions are bridged to `PADCHORD` automatically, gated by a framebuffer pixel fingerprint that detects the Pads page and the Enable Pad Play / Chord Assign toggle states. See api.md §7 (`PADCHORD` and `PADMAP*`) for the full calibration and gating details.

`PALETTE`, `REGION`, and `PIXEL` are framebuffer-read diagnostic/calibration commands used to derive the pixel fingerprints and hit boxes above; they have no dependency on `nks4_inject.ko`.

### MIDI injection

Because `midi_bridge.ko` reads OA's in-memory objects (the MIDI in-port array and the out-queue it taps as a reader), its load is **deferred until the Kronos has finished booting** — specifically until EVA has drawn its UI, detected from the framebuffer (non-black percentage plus distinct-colour count; the loading/update screen is essentially all-black with a couple of colours, the drawn UI is not). This is well after OA reaches `MODULE_STATE_LIVE`, so the module never reads half-built state, and — critically — it keeps the tap out of the fragile boot-settling window (see "Real-time safety" below). A `/korg/rw/screenremote/.fbcurve` flag file puts the daemon in a brick-safe calibration mode that logs the boot framebuffer curve and skips loading `midi_bridge` entirely. Unlike its predecessor `midi_inject.ko`, the module does **not** patch OA `.text` — MIDI-out capture is done by claiming a spare reader slot on OA's transmit queue, so there is no trampoline, no `.text` write, and no teardown freeze when OA is unloaded.

**Real-time safety.** OA's transmit queues live in the codec's real-time-critical memory, so continuously tapping them is safe only because the daemon and its `midi_tcp` subprocess pin themselves to a CPU core away from the audio engine's core (`sched_setaffinity`) — otherwise framebuffer streaming crowds the real-time engine during the boot-settling window and freezes the UI. That pinning, plus deferring the module load until the UI is up, is what lets screen mirroring and continuous MIDI capture run at once without disturbing the synth. (A `tap_shared=0` module option trades away live-performance capture to read the codec queues only inside a request-scoped dump window, for a minimal-footprint dump-only build; it is not the default.)

At startup the daemon also loads `midi_bridge.ko` (embedded as `midi_bridge_ko.h`) with `init_module(2)`, passing kernel symbol addresses for `MidiInPortGeneric7Receive` and `RegisterMidiInPort` resolved from `/proc/kallsyms`. The module creates two proc entries:

- `/proc/.midi_in` - write raw MIDI bytes to inject into the Kronos MIDI engine (OA.ko)
- `/proc/.midi_ring` - read captured MIDI-out (SysEx responses and bulk dumps) staged in the module's ring
- `/proc/.midi_ports` - diagnostic counters (tap state, `overflow`, `inject_ok`, `drain_open`)

The daemon then spawns an embedded `midi_tcp` subprocess (built from `source/midi_tcp.c`, embedded as `midi_tcp_bin.h`) that listens on TCP port 9875. This subprocess is a single-threaded MIDI bridge: it writes inbound TCP bytes directly to `/proc/.midi_in` and reads MIDI output from `/proc/.midi_ring` (polling slowly at idle and bursting only while a reply flows), forwarding messages to TCP as they arrive. Channel and system-common messages are forwarded whole; SysEx is **streamed incrementally in ≤1 KB chunks** so an arbitrarily large object (a full Set List dump is ~79 KB) crosses the bridge with no size cap — clients reassemble `F0…F7` across chunk boundaries. The outbound stream is a single generic feed carrying **everything the Kronos transmits** — live performance (notes, CC, program/combi change, aftertouch, pitch bend) **and** SysEx responses and bulk dumps — destination-agnostic and de-duplicated (see api.md §8.2). A `tap_shared=0` build drops performance capture for a dump-only, minimal-RT-footprint mode; it is not the default. The daemon connects to the subprocess on localhost and uses it to handle `MIDI_SEND`, `SYSEX`, and `MIDI_STATUS` control commands. Responses may arrive interleaved with other MIDI, so clients match them to requests by inspecting the SysEx payload (Korg manufacturer ID 0x42, model ID 0x68, and function code) rather than by timing or stream position.

If the kernel symbols cannot be resolved (e.g. the Kronos OS version is too old or the symbols are stripped), MIDI injection is silently disabled and `MIDI_STATUS` will report `MIDI_LOADED=0`.

Continuous controllers (e.g. mod wheel, breath) sent via `MIDI_SEND` are rate-limited per `(status, controller)` to at most one injection every 7 ms, keeping only the latest value; the final position is always delivered once the controller stops moving. This prevents a fast controller sweep from flooding OA's MIDI queue and delaying other MIDI (e.g. pitch bend) stuck behind it. Notes, pitch bend, and SysEx are never throttled.

### Boot safety and recovery

Kernel module loading (`vkbd.ko`, `nks4_inject.ko`, `midi_bridge.ko`) is inherently risky on the Kronos's RTAI kernel, so startup is guarded by three independent mechanisms, all living under the FTP-visible `/korg/rw/HD` folder so they are reachable even on a non-rooted unit with no shell access:

- **Boot flag** (`/korg/rw/HD/ScreenRemote/.boot`) - written at the start of every boot and deleted only once the framebuffer, network, and both listeners are confirmed up. If it is still present when the daemon starts, the previous boot did not finish cleanly, so no kernel modules are loaded that boot. Delete the file over FTP to re-enable module loading on the next boot.
- **Kill switch** (`/korg/rw/HD/_nomod`) - if this folder exists, the daemon loads no kernel modules at all, regardless of the boot flag. Create it over FTP (`mkdir _nomod`) and reboot to bring the unit up with no kernel modules at all - useful before running a Korg OS update or the Factory State Restore cleaner, and for recovering a wedged unit. (`midi_bridge.ko` no longer patches OA `.text`, so the old teardown freeze at "Preparing to Install" is gone; this kill-switch remains as defense-in-depth.)
- **Boot kernel-log capture** (`/korg/rw/HD/ScreenRemote/boot_kmsg.log`) - stock (non-rooted) units have no dmesg/klogd, so the daemon snapshots the entire kernel ring buffer every 150 ms across the risky module-load window and fsyncs each snapshot. If a module load freezes the box, the last fsync'd snapshot still holds the kernel messages up to the hang. Capture stops, with one final snapshot, once startup completes successfully.

---

## Building

### Prerequisites

- `i686-linux-gnu-gcc`, or plain `gcc` with 32-bit multilib support (`gcc-multilib`) - the daemon and `midi_tcp` build with `-m32` when no dedicated i686 cross-compiler is found
- `make`
- `xxd` - used to convert `vkbd.ko` into the `vkbd_ko.h` C array

**To rebuild `vkbd.ko` / `nks4_inject.ko` / `midi_bridge.ko` from source** (not required if you use the pre-built modules):

- **[`cgudrian/linux-kronos`](https://github.com/cgudrian/linux-kronos)** (branch `v2.6.32.11-kronos`), built with Korg's own `arch/x86/configs/korg_kronos_defconfig` and prepared for out-of-tree module builds (`make ARCH=i386 oldconfig && make ARCH=i386 prepare scripts modules_prepare`). This is Korg's actual kernel source, not a vanilla 2.6.32.11 tree - modules must be built against it to match the real kernel's `struct module` ABI (see `patch_init_offset.py` below for what goes wrong otherwise, and `../project_linux_kronos_kernel_tree.md` for how this tree was found/validated and exactly how to (re)prepare it).
  - `Makefile.module` in each module dir defaults `KDIR` to `/home/build/linux-kronos`; override with `make KDIR=/path/to/linux-kronos` if yours lives elsewhere. **Build it on a filesystem that supports real symlinks** - `modules_prepare` creates `include/asm -> include/asm-x86`, which fails on CIFS/SMB mounts even with `symlink=native` set.
  - A plain vanilla `linux-2.6.32.11` tree can still be used in a pinch (`make KDIR=/path/to/linux-2.6.32.11`) - `patch_init_offset.py` will patch the one offset it knows about, but the rest of `struct module`'s layout (e.g. the percpu `refptr` field) will still be wrong relative to the real kernel. Prefer `linux-kronos`.
- **Python 3** - required to run `patch_init_offset.py` during the module build (see below).

### Step 1 - build the kernel modules

```sh
make -C vkbd_module -f Makefile.module
make -C nks4_inject_module -f Makefile.module
make -C midi_module -f Makefile.module
```

Each runs `make modules` against the 2.6.32.11 kernel tree, runs `patch_init_offset.py` on the resulting `.ko`, and then uses `xxd -i` to regenerate the corresponding C header (`source/vkbd_ko.h`, `source/nks4_inject_ko.h`, `source/midi_bridge_ko.h`).

Step 1 is required - the `.ko` files and their generated headers are not committed and must be built before Step 2.

### Step 2 - build the daemon

```sh
make -C source
```

Output: `build/screenremote` - a statically linked i686 ELF binary.

---

## patch_init_offset.py

The Kronos kernel's `struct module` places the `init` function pointer at offset `0xd4`. A module built against a vanilla Linux 2.6.32 tree (mismatched CONFIG options relative to Korg's real kernel) places it at `0xbc` instead. If such a module is loaded unpatched, the kernel writes the `init_module` address to the wrong offset and the module's init function is never called - and, worse, if that module's init ever needs to signal failure, the kernel's stock error-cleanup path (`module_put()` on the misshapen module) reads other kernel-populated fields like the percpu `refptr` from the wrong offset too, which oopses the kernel rather than just failing the load.

Building against `linux-kronos` (see Prerequisites above) avoids this at the source: `init` naturally lands at `0xd4` with no patching, verified byte-for-byte against real Korg-shipped `.ko` files (not just that one field - the whole `.gnu.linkonce.this_module` section matches in size). `patch_init_offset.py` still runs automatically after every module build as a cheap defensive check - it detects the offset is already correct and no-ops - so it also still fixes the one symptom it knows about if someone builds against a mismatched tree. It finds the `.rel.gnu.linkonce.this_module` section, locates the `init_module` relocation entry, and changes its `r_offset` from `0xbc` to `0xd4`. It applies to `vkbd.ko`, `nks4_inject.ko`, and `midi_bridge.ko`.

**Requirements:** Python 3 (standard library only - `struct`, `sys`, `os`).

**Manual use:**

```sh
python3 patch_init_offset.py vkbd_module/vkbd.ko
python3 patch_init_offset.py nks4_inject_module/nks4_inject.ko
python3 patch_init_offset.py midi_module/midi_bridge.ko
```

---

## MIDI injection history

An earlier approach (`vusb_midi/`, since removed) attempted to use `dummy_hcd` to inject MIDI messages via a virtual USB gadget interface. This was abandoned because `dummy_hcd` is incompatible with the Kronos's RTAI real-time kernel - the URB completion tasklets conflict with RTAI scheduling and cause kernel panics. The current approach calls the Kronos MIDI receive function directly from a kernel module (`midi_module/midi_bridge.c`) and bridges SysEx request/response via an embedded TCP subprocess.

**1.9.0 → 1.9.2.** 1.9.0 replaced the old `.text` trampoline hook with the hook-free reader-slot tap. 1.9.1 added fast USB-routed dump replies but proved unstable: it intermittently froze the Kronos (EVA/OA) in the first few seconds after boot. The cause turned out to be **CPU-core contention**, not the MIDI logic — the OS was scheduling framebuffer streaming onto the same physical core as the RTAI audio engine and the boot-time PCM sample loader, and during the boot-settling window that starved the real-time engine. 1.9.2 fixes it by pinning the daemon and `midi_tcp` off the audio core and deferring the module load until the UI is up. With the contention gone, the full generic capture (live performance **and** dumps, continuous) runs stably alongside screen mirroring; a `tap_shared=0` option remains for a minimal-footprint dump-only build. *(During the hunt the tap was also hardened — claim-on-demand slots, injection-gated draining — before the real cause was found; those paths survive as the `tap_shared=0` mode.)*

---

## Front-panel injection history

Earlier versions injected `TOUCH*`/`BUTTON`/`CHORD`/`WHEEL`/`SLIDER`/`VSLIDER` events by writing synthetic packets to `/dev/rtf5`. That worked for touch and mode-select buttons but never reliably drove sequencer transport, tempo, or some MIDI-triggering touch actions, because `/dev/rtf5` is actually OA.ko's own **outbound** notification channel to Eva, not an input path — a synthetic packet only ever fooled Eva's UI-mirroring logic, never the real OA-side action.

**1.10.0** replaced this with `nks4_inject.ko`, a small companion kernel module that calls OA's real `CSTGFrontPanel::HandleSwitchEvent` / `HandleTouchPanel` / `HandleRotary` / `HandleAnalogController` directly via `/proc/.nks4inject` — the exact functions a physical press/touch/turn dispatches through. This also enabled a batch of previously-unavailable controls: `KNOB`, `JOYSTICK`, `VECTOR`, `RIBBON`, `AFTERTOUCH`, `PEDAL`, `FOOTSWITCH`, `DAMPER`, and `TEMPO`, all hardware-confirmed against a real unit (see api.md §7 and §10).

**Pads (touch to play) bridge.** The on-screen Pads grid turned out not to dispatch through `HandleTouchPanel` like every other touch widget — tapping it instead feeds a separate, single shared KARMA CC trigger. The real per-pad mechanism, `RT_chord_trigger`, was found via a one-shot diagnostic `.text` hook (`chord_probe.c`, not part of the shipped daemon) and is now called directly by the `PADCHORD` command. `PADMAP`/`PADMAP_ON` bridges real taps on the Pads page to `PADCHORD` automatically, gated by a framebuffer pixel fingerprint (`PALETTE`/`REGION`/`PIXEL` were added as general-purpose calibration commands to derive it) plus the Enable Pad Play / Chord Assign / Fixed Velocity toggle states. See api.md §7 (`PADCHORD` and `PADMAP*`) for the full calibration history and gating logic.

---

## Network robustness (1.11.1 → 1.11.2)

A real kernel oops was captured on hardware during a Korg OS update while the daemon was running: `midi_bridge.ko`'s own OA-unload notifier dereferenced a stale pointer into OA's MIDI queue-control memory (that memory can be freed/reallocated by OA during ordinary operation, e.g. a Program/Combi load, not just at module unload — a plausible-looking-but-freed address isn't something the existing pointer sanity check can catch). Fixed by routing every touch of that memory through the kernel's fault-safe `probe_kernel_read`/`probe_kernel_write` primitives instead of raw dereferences, so a stale pointer now drops cleanly instead of oopsing. Confirmed fixed by watching the exact crash path complete cleanly live, through a real update, on real hardware.

Alongside that, a module-unload-orphaning bug (`nks4_inject.ko` could be left resident with a live OA-unload notifier still registered after certain shutdown paths) and a missing `FD_CLOEXEC` on the daemon's sockets (an orphaned `midi_tcp` subprocess after a crash could hold the listen ports, blocking a restart) were both fixed — internal robustness, not visible on the wire.

**Client-visible behavior changes**, all documented in their respective api.md sections above: the control port now enforces a 1-second total deadline on a connection's first command line (previously only a per-byte timeout, which a slow-trickling client with no newline could exploit to freeze the whole daemon indefinitely) and a 2-second send timeout on replies; a `CTRL_PERSIST` session's ownership is now continuously re-validated instead of only at connect time; and `PADMAP_OFF` / an overlapping `TOUCH_DOWN` now reliably send a chord's Note-Off before dropping it internally, backed by a 10-second watchdog that force-releases a held pad if a client disconnects mid-hold without ever sending `TOUCH_UP`. None of this requires a client to change how it talks to the daemon — these are robustness fixes, not new requirements.

A protocol-aware fuzzing/stability regression suite (`tests/network_fuzz/`) was built to hunt for crashes across all three network surfaces — malformed handshakes, malformed control commands, malformed UDP discovery packets, plus true-concurrency and stateful-sequence attacks targeting the fixes above specifically. Ran clean (no crash, no fd leak) across ~130 test-case executions on real hardware; see its README for usage if you're extending the protocol and want to check your changes the same way.

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

**Embedded binary blobs.** The daemon contains four binaries compiled into it as raw byte arrays (`vkbd_ko.h`, `nks4_inject_ko.h`, `midi_bridge_ko.h`, `midi_tcp_bin.h`) and extracts and loads them at runtime - the kernel modules via the `init_module(2)` syscall directly, and the MIDI bridge via `fork()`/`execl()`. Self-extracting executables that carry and load additional binaries are a well-known malware distribution technique; heuristic scanners flag the pattern regardless of intent. The reason for doing this here is that `system()` and `/bin/sh` are unavailable on non-rooted Kronos units.

**Direct kernel module loading.** Calling `init_module(2)` to load kernel modules without going through `/sbin/insmod` is unusual enough to be a trigger on its own. Rootkits use the same syscall to load modules that hide processes or intercept system calls. The modules here only register a virtual keyboard with the input subsystem and provide front-panel and MIDI injection interfaces.

**Input event injection.** Writing to `/dev/uinput` and `/proc/.nks4inject` to inject keystrokes, touchscreen, and front-panel control events is a behavioral signature of keyloggers and Remote Access Tools. This is exactly the intended use here - remote control of the Kronos UI from a client application.

**Statically linked binary.** The binary has no shared library dependencies. Statically linking is common for embedded targets (the Kronos has a minimal filesystem), but it is also a common malware technique for portability and to avoid detection via library monitoring.

**Screen capture over TCP.** The core function of the daemon - reading the framebuffer and streaming it over an authenticated TCP connection - matches the behavioral profile of a Remote Access Tool or spyware at the network level. It is, intentionally, a remote screen viewer.

Taken individually each of these patterns has a legitimate explanation. Taken together on a single statically linked binary they push most heuristic classifiers well past their detection threshold. If you need to share the binary through a service that blocks it, archiving it in a password-protected zip (password noted in accompanying text) is usually sufficient to pass it through, since the scanner cannot inspect the contents.
