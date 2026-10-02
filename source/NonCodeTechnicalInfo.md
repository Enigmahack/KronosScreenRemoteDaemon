# ScreenRemoteDaemon Non-Code Technical Information

This document supplements TechCodeManual.md with technical details for kernel modules, utilities, and diagnostic tools that don't warrant their own major sections but contain important design decisions and constraints.

## 1. Fault-Safe Memory Access (oa_safe.h)

### Problem

Both nks4_inject.ko and midi_bridge.ko read/write memory whose lifetime they don't control:
- OA.ko's .text (immutable at runtime, but OA can be unloaded)
- Codec-owned ring-buffer control structures (OA can free or move during normal operation, e.g., during Program/Combi load codec-MIDI reconfiguration)

A raw dereference of a since-freed or since-moved address causes an oops.

### Solution

Use `probe_kernel_read()` and `probe_kernel_write()` which route every dereference through the kernel's fault exception tables, converting unmapped-address oopses into clean failures.

### kptr_ok() Limitations

`kptr_ok()` only rejects obviously-garbage pointers (null, misaligned, out-of-kernel-range). It cannot distinguish a plausible-but-stale pointer from a valid one, so every actual dereference still goes through probe_kernel_* instead of trusting kptr_ok() alone.

### History

Originally two copies (nks4_inject.c and midi_bridge.c), byte-identical where they overlapped. Merged into this shared header to ensure fixes land in both modules. Each module keeps its own call-site macro names (oa_read8/tap_read8) to avoid changing every call site.

---

## 2. Virtual Keyboard Module (vkbd.ko)

### Design Challenge

Eva's `CHIDDriver::KeyboardIsConnected()` scans `/sys/class/input/event0..event3` for the first device with `id/bustype` = "0003" (BUS_USB) and locks onto it. It never rescans while the bound device's sysfs node exists.

When a real USB keyboard is already attached at power-on, it enumerates during early boot (before the daemon starts) and claims a lower event slot than vkbd. Eva finds the real keyboard first and stays bound to it, leaving vkbd outside the scanned range and invisible.

### Solution: Input Handler Relay

Instead of fighting for a slot, vkbd tracks (via `input_handler`) whichever device Eva is currently bound to (real or vkbd itself) and injects into THAT device. Since `input_event()` delivers to every handler attached to a device, this reaches Eva regardless of which device it's bound to.

### Sticky Target Selection (Critical)

Target selection must be sticky to avoid replacing Eva's bound device when a new keyboard hotplugs:
- **Initial scan** (during `input_register_handler()`, before Eva starts): mirrors whatever Eva's own first scan finds
- **Slot reclaim** (when the actively-targeted device disconnects): reclaim vkbd's slot by unregistering and re-registering vkbd_dev
- **Other hotplug events**: record but never change the active target (mirrors Eva's "never rescans just because something new appeared" behavior)

A previous wrong implementation would preempt to a newly-plugged keyboard even after vkbd had become Eva's active device, causing keystrokes to go nowhere (the real keyboard and vkbd both existed but Eva read neither).

### Slot Reclaim Mechanics

vkbd_dev registers once at module load. If that happens while a real keyboard already holds Eva's scanned range, vkbd's slot is permanently outside it.

`vkbd_reclaim_fn()` unregisters and re-registers vkbd_dev the moment the actively-targeted device disconnects and no other external candidate remains. This lets vkbd claim whichever slot just vacated, the same slot Eva's own rescan is about to search in.

Must run from a workqueue (not directly from disconnect()): both `input_unregister_device()` and `input_register_device()` hold the global `input_mutex`, and disconnect() callbacks are invoked while that mutex is already held, causing self-deadlock.

### Test-Only Parameter

`setup_delay_ms`: delays deferred setup to make an unload-vs-setup race condition deterministic. Exists purely to test/reproduce the kernel_safety/t3_vkbd_leak race condition.

---

## 3. Eva Mode State Module (eva_mode.ko)

### Purpose

Reads Eva's live `CModeManager` state directly from process memory. Exposes this via `/proc/.eva_mode` as the primary source for screenremote's MODE/EDITCTX reporting (STATE, SYSINFO, MODE_DETAIL commands).

### Data Flow

`sm_poMMI` (Eva's global) → `CMMI::modeManager` → `CModeManager` fields

All field offsets (#defines, not module params) are calibrated live against pixel ground truth.

### Raw vs. Translated Output

Deliberately reports RAW `SYS_MODE` (0-6, Eva's own `ESysMode` ordinal) and RAW `EDITCTX_RAW` (0-2), NOT the translated 1-7 / 0-2 values screenremote uses on the wire. Translation lives in screenremote.c (`eva_mode_read()`) so mapping fixes only need a daemon rebuild, not a kernel module reload.

### Task List Search

Uses RCU-only task-list walk (no `tasklist_lock`, no `get_task_struct` - neither are EXPORT_SYMBOL on this kernel). Matches on two criteria:
1. Task comm name (default "Eva")
2. Substring in resolved exe path (default "/Eva/Eva")

Picks lowest PID among matches (tiebreak for transient same-comm or same-exe processes).

### STAGE= Diagnostics

Added to distinguish why `RESOLVED=0` occurs on a console-less production unit:
- `find_task`: no process found matching eva_comm or eva_exe_path
- `read_sm_pommi`: Eva found but sm_pommi_addr doesn't resolve
- `read_modemgr_ptr`: pointer chain broken at CModeManager hop

Without this, both "Eva hasn't started yet" and "address recalibration needed" collapsed into a bare `RESOLVED=0` with no way to disambiguate without interactive diagnostics (not possible on console-less units).

### sm_pommi_addr Parameter

Writable via `/sys/module/eva_mode/parameters/sm_pommi_addr` (0644 permissions). screenremote.c auto-detects the real value by reading Eva's ELF .symtab and live-corrects it via sysfs the moment Eva.img's cryptoloop mount exists. The compiled-in default (0x0ae431b0) is calibrated against Eva 3.2.2 only and is a fallback.

---

## 4. Mode/Page Button LED State (mode_page_hook.ko)

### Purpose

Permanent read-only hook on `HandleSwitchEvent` that counts MODE/PAGE button presses, providing the press trigger for Nautilus LED state detection (screenremote's `lit_state()` function).

### Nautilus Only

These popup-toggle buttons don't exist on Kronos, which has discrete mode-select buttons monitored by eva_mode.ko instead. The button codes happen to collide (COMBI=1, PROGRAM=2), so the hook is conditionally loaded only on Nautilus.

### Hook Safety

Uses the same locked `cmpxchg8b` single-instruction replacement strategy as nks4_inject.ko (see TechCodeManual §11). Replaces one whole 5-byte instruction at a fixed offset, refusing install if the first 11 bytes don't match or the function address isn't 8-byte aligned.

---

## 5. Palette Data Generation (palette_data.h)

### Automatic Generation

Auto-generated from Eva's live framebuffer snapshot using `tools/extract_boot_splash.py`. Contains the 256-color RGB888 palette used for 8bpp palette-indexed framebuffers (Kronos family).

Not applicable to Nautilus (16bpp RGB565 truecolor) - the palette is shipped hardcoded for historical reasons but never used.

---

## 6. Mode Detection via Pixel Fingerprints (mode_detect_refs.h)

### Fallback Source

When eva_mode.ko isn't loaded or hasn't resolved yet, screenremote falls back to pixel detection: comparing framebuffer regions against known UI signatures for each mode.

References compiled from real hardware pixel captures, baked into this header. Includes thresholds and region coordinates for each mode's distinguishing UI elements.

### Secondary Fallback

When pixel detection also can't decide, falls back to the last mode commanded by the client via BUTTON commands.

---

## 7. LED State Detection (lit_detect_refs.h)

### Nautilus Only

Detects MODE/PAGE button LED state by analyzing on-screen popup pixel patterns. The popups appear at fixed coordinates and use consistent colors; analysis watches for the popup frame and background to determine LED state in real-time.

Updated once per frame when a change-mode v3 client is actively diffing, or on-demand via explicit REFRESH command.

---

## 8. Diagnostic Modules

### Chord Probe (chord_probe.c)

Traces `RT_chord_trigger` calls to inspect pad-trigger behavior in real-time.

### Eva Mode Peek (eva_mode_peek.c)

Diagnostic counterpart to production eva_mode.ko. Includes full pointer-chain provenance documentation and stage-by-stage debugging output. Used to calibrate eva_mode.ko field offsets.

### LED Hook / NKS4 LED Read

Hooks or directly reads LED state from the panel link for real-time LED observation during development.

### OmapVideo Peek (omapvideo_peek.c)

Direct framebuffer peek without the screenremote daemon overhead. Used for low-level display debugging.

### NKS4 Colorpal Capture (nks4_colorpal_capture.c)

Captures NKS4 subsystem firmware's color palette table for palette investigation.

### SHM Peek (shm_peek.c)

Inspects shared memory regions directly for OA subsystem debugging.

---

## 9. MIDI TCP Bridge (midi_tcp.c)

### Purpose

Userspace bridge between screenremote's `/proc/.midi_ring` (kernel MIDI capture) and remote clients. Connects via TCP and streams MIDI OUT events from the Kronos to connected clients.

### Protocol

Binary stream of MIDI bytes (no framing). Clients connect and read until EOF (server closes on daemon exit or MIDI device unload).

---

## 10. Boot Splash Handling

### Optional Feature

Compile-time boot-splash fallback (see `tools/extract_boot_splash.py`). On systems with an embedded `boot_splash_data.h`, a splash screen is composited onto early frames while the boot gate is active. Normal case (fresh checkout, no generated file) builds identically to before this feature existed.

---

## 11. Utilities and Tools

### Diagnostic Modules Framework

All diagnostic kernel modules follow the same pattern:
- Optional `/proc` interface for reading/writing
- Deferred setup via `schedule_work()` (not from `init_module` directly)
- Workqueue-based since direct `create_proc_entry()` fails on RTAI kernel
- OA-unload notifiers to prevent calls into freed module memory

### EvaTrace (evatrace.c)

Real-time tracing of Eva subsystem calls. Useful for understanding execution flow during specific operations.

### Memory Dump Utilities (memdump.c)

Low-level memory inspection tools for debugging subsystem state.

---

## 12. Kernel Safety Tests (tests/kernel_safety/)

### t2_inject_latency.c

Measures latency of front-panel event injection across various system load conditions.

### t3_vkbd_leak.c

Reproduces the unload-vs-setup race condition in vkbd (requires `setup_delay_ms` parameter to deterministically trigger).

### oaghost.c

Tests OA module unload and reload safety with various subsystem states.

---
