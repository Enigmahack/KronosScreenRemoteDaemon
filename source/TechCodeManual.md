# ScreenRemoteDaemon Technical Code Manual

This document houses architectural explanations and non-obvious technical details referenced from the source code. Each section is mapped to the file(s) it relates to.

## 1. Nautilus 16bpp Framebuffer Architecture (screenremote.c)

### The Problem

Kronos and Nautilus handle framebuffers differently:

- **Kronos**: `fb1` is genuinely 8bpp palette-indexed; Eva sets the palette and all apps read/write indices.
- **Nautilus**: `OmapVideoModule` defaults to 16bpp; Eva renders RGB888 into a virtual framebuffer and writes true RGB565 little-endian truecolor pixels directly to the hardware buffer.

The kernel's FBIOGET_FSCREENINFO reports stale 8bpp metadata (inherited from Kronos) on both families, but Nautilus actually contains real 16bpp RGB565 data.

### The Solution

The daemon maintains two separate mappings:

1. **fb1_hw_map/fb1_hw_stride** - The raw device mapping (16bpp on Nautilus, 8bpp on Kronos)
2. **fb1_shadow** - A synthetic byte-per-pixel shadow buffer, refreshed once per tick with the LOW byte of each RGB565 word (GGGBBBBB)

This shadow is NOT a palette index; it's a luminance-ish proxy used only by the non-streaming heuristics (boot gate detection, screensaver sampling, VGA mirror, mode detection) which consume it exactly as in version 3.0.1, preserving their Nautilus behavior.

The v3 stream reads `fb1_hw_map` directly, delivering native RGB565 pixels to clients.

v2 clients are refused on Nautilus hardware rather than fed the shadow byte as an incorrect palette index (which is what version 3.0.1 did, causing wrong colors).

**See also:** `fb1_refresh_shadow()` implementation.

---

## 2. Touch Calibration Mesh (screenremote.c)

### Physical Mismatch on Nautilus

Eva's touch handling converts ADC back to pixels in an 800x600 logical canvas on BOTH Kronos and Nautilus families (hardcoded `/800` and `/600` in Eva's `CEditor::CPanelIfcTask::OnTouchPanelEvent`). 

Horizontally the Nautilus canvas reaches the physical panel 1:1, so horizontal calibration is Kronos-compatible.

Vertically the Nautilus applies `KorgDisplayBilinearScalerSSE` that compresses the 800x600 canvas to PegRect{0,0,799,521}, shrinking a canvas row by ~522/600 before it reaches the physical display rows the daemon streams. The vertical constants absorb this compression ratio.

### Calibration Constants

Both families share `NAUTILUS_TOUCH_X_RANGE=857, NAUTILUS_TOUCH_X_OFFSET=24`. The vertical pair differs and only applies to Nautilus: `NAUTILUS_TOUCH_Y_RANGE=570, NAUTILUS_TOUCH_Y_OFFSET=29`.

The daemon computes ADC values as:
```c
h_adc = (x - g_touch_x_offset) * 255 / g_touch_x_range
v_adc = (y - g_touch_y_offset) * 255 / g_touch_y_range
```

Eva then scales these independently with its own margin pairs and spans (from `sm_aucTouchPanelMargin` in Global > Touch Panel Calibration).

**Caveat:** Running on-device Eva calibration modifies `sm_aucTouchPanelMargin` and invalidates these constants.

---

## 3. Frontend Injection Architecture (screenremote.c)

### Why Two Mechanisms

The daemon supports two injection paths:

1. **nks4_inject.ko** (preferred) - Calls OA's real `CSTGFrontPanel` handlers directly:
   - `HandleSwitchEvent` (button press/release)
   - `HandleTouchPanel` (touch events)
   - `HandleRotary` (knob/wheel)
   - `HandleAnalogController` (sliders, joystick, ribbon, etc.)
   
   Events receive identical OA-side processing as hardware, independent of Eva mode.

2. **rtf5 fallback** (degraded) - Writes synthetic packets to `/dev/rtf5`, OA's OUTBOUND FIFO to Eva.

### Why rtf5 is Limited

rtf5 only reaches Eva's UI-mirroring code, not OA's actual event handlers. This explains why:
- **Touch/mode buttons** mostly work (Eva reconstructs behavior from the mirrored packet)
- **Sequencer transport** (SEQ_START/SEQ_REC/SEQ_LOCATE/SEQ_FF/SEQ_REW/SEQ_PAUSE), **TAP_TEMPO**, **SMPL_REC/SMPL_START** silently do nothing (real effect lives in OA, never reached)
- **KNOB/JOYSTICK/VECTOR/RIBBON/AFTERTOUCH/PEDAL/FOOTSWITCH/DAMPER/TEMPO/PADCHORD** have no rtf5 equivalent and are unavailable in fallback mode

Fallback is logged to stderr and `RTF5_FALLBACK_LOG` so a degraded boot is never silent.

---

## 4. MIDI OUT Capture Mechanism (midi_bridge.c)

### The Approach

Instead of trampoline-hooking `CSTGMidiOutPort::ReadNextMessage`, the module claims spare reader slots on OA's source queues and drains them:

- **Shared performance queues** (q1, q2): notes, CC, program-change, combi SysEx (identical across all out-ports; tapped once)
- **Per-port bulk-dump queue** (q3): destination-specific bulk data (USB ~1 MB/s, DIN ~3.6 KB/s)

This produces one destination-agnostic stream: performance data appears once (per-port queues don't echo it), and any dump to any destination is captured. q0 (active-sensing) is excluded.

### Reader Slot Management

OA registers exactly 2 out-ports (fixed at compile time), so each queue has stable reader counts with free slots. Because OA never grows the count at runtime, the module is always the top reader on each queue. On unload, slots are atomically released (no leak, clean reload).

### Flow Control

An added reader participates in the ring's drop-on-full free-space calculation. A slow consumer could throttle OA's real output, so the drain is best-effort: when behind, we skip the cursor forward, dropping our copy instead of stalling OA.

### Window-Based Polling

To avoid contention with the RT engine:
- Polling happens ONLY inside a window opened by a dump request (MidiInPortGeneric7Receive injection)
- Window stays alive while reply bytes flow (`DRAIN_OPEN_MS=3000`, kept short for non-dump injections)
- Outside the window, `tap_drain` touches no codec memory at all

---

## 5. Boot-Safety Flag (/korg/rw/HD/ScreenRemote/.boot)

### Purpose

Written at startup, deleted only after framebuffer, network, and listeners are all up AND every deferred module load reaches a terminal state.

If the flag exists on entry, the previous boot did not complete cleanly, so kernel modules are skipped for that boot and the unit comes up module-free.

### Recovery Behavior

The flag is CLEARED on any successful startup, including module-free recovery boots. This is deliberate: an intermittent module bug costs one degraded boot and then self-heals, rather than latching the unit into permanent module-free mode.

The tradeoff is a deterministic boot-hang (e.g., an OA build midi_bridge can never attach to) produces an alternating brick → recovery → brick cycle. Every other boot is working and FTP-reachable, making the escape hatch usable.

### Kill-Switch

To break the cycle permanently, create `/korg/rw/HD/_nomod` over FTP during a recovery boot. This disables module loading on EVERY boot until the folder is removed. Manually deleting `.boot` only skips the current boot's guard; it doesn't make a fix stick.

---

## 6. Device Model Detection (screenremote.c)

### FAMILY Determination

`FAMILY` is derived from `fb1`'s native bpp (hardware-confirmed):
- 8bpp → KRONOS
- 16bpp → NAUTILUS

### MODEL Codes

Within each family, `MODEL` is narrowed by CPU string from `/proc/cpuinfo`:

**KRONOS family:**
- KRONOS1 (Atom D510)
- KRONOSX (Atom D525)
- KRONOS2 (Atom D2550)
- KRONOS3 (Celeron J3160) — inferred conflict resolution, logged as uncertain
- KRONOS_UNKNOWN (unrecognized CPU)

**NAUTILUS family:**
- NAUTILUS (recognized CPU)
- NAUTILUS_UNKNOWN (unrecognized CPU) — inferred conflict resolution, logged as uncertain

These are static for the life of the process.

---

## 7. MODE/PAGE LED Detection (Nautilus, screenremote.c)

The MODE and PAGE button LEDs are detected by analyzing the on-screen popup each one opens, not by counting button presses. This prevents drift when popups close via touch or a press is ignored.

The mechanism reads framebuffer pixel patterns from `lit_detect_refs.h` to determine LED state in real-time.

---

## 8. CPU Affinity Management (screenremote.c)

### Why It Matters

On Nautilus, the single USB2 NIC dongle shares the xHCI bus (and its IRQ core) with the NKS4 front-panel link. An uncapped burst of streaming frames can crowd the front-panel itself.

The daemon detects CPU topology (cores, threads, RTAI task cores) and pins itself and MIDI tasks to cores separate from RTAI real-time tasks, reducing contention.

Override with `cpu_affinity=` in `screenremote.cfg` (bitmask, 0 = auto).

---

## 9. State Reporting Hierarchy (screenremote.c)

### MODE and EDITCTX

The `STATE` command reports live mode and edit context:

**Primary source:** `eva_mode.ko` (if loaded) reads Eva's `CModeManager` state directly from process memory. Exact, no thresholds. Only source that reports EDITCTX=2.

**Fallback:** Pixel detection (`detect_ui_mode()`, `detect_program_edit_context()`, `mode_detect_refs.h`) when eva_mode.ko isn't loaded or hasn't resolved yet (e.g., early boot).

**Secondary fallback:** Last mode commanded via `BUTTON` command (pixel detection only).

Either way, this is the daemon's own source of truth, not an echo of client-side comparison.

**Values:**
- MODE: 0=init/undetected, 1=Setlist, 2=Combi, 3=Program, 4=Sequence, 5=Sampling, 6=Global, 7=Disk
- EDITCTX: 0=none, 1=Program-edit-from-Combi, 2=Program-edit-from-Sequence
- EDITSLOT: timbre/track index being edited (-1 if EDITCTX=0 or eva_mode.ko unresolved)

---

## 10. Byte-Rate Capping (screenremote.c)

A token-bucket rate limiter on Nautilus (24 Mbps default, overridable via `stream_max_kbps=` in config).

On Kronos (PCIe NIC, separate TWI panel bus), the default is unlimited (0). This prevents USB-MIDI and framebuffer bursts from crowding the Nautilus front-panel link, which shares the xHCI bus.

---

## 11. nks4_inject.ko Hooking Strategy

### Why Trampolines Matter

On 32-bit x86, a 6-byte memcpy compiles to a 4-byte write then a 2-byte write. With preemption enabled, another CPU could run the jmp before its target address is fully written. The old approach also overwrote three prologue instructions with one jmp, creating instruction-boundary hazards.

### Solution

The hook now replaces exactly one whole 5-byte instruction at `HandleSwitchEvent+0x0a`: a `mov eax,[abs32]`. The target addresses at +0x0a are untouched, no branch lands inside the rewritten bytes, and every CPU sees either the whole original instruction or the whole jmp, never a mix.

Install and removal each rewrite the aligned 8 bytes around it in one locked write (`lock cmpxchg8b`).

The module:
- Refuses to install if the first 11 bytes don't match expected bytes or the function address isn't 8-byte aligned
- Shares one memory block for trampoline + counters, never freed
- A task paused inside the trampoline during `rmmod` finds valid code when it resumes

---
