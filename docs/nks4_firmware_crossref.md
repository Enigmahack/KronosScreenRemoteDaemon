# Cross-reference: NKS4 panel firmware (`KRONOS_V06R06.VSB`)

A disassembly pass of the panel controller's own firmware image
(`Decomp/subsystem/KRONOS_V06R06.VSB` in the sibling `kronosology` repo — full
writeup: [`kronosology/docs/modules/KRONOS_V06R06.VSB.md`](../../kronosology/docs/modules/KRONOS_V06R06.VSB.md))
turned up a few things directly relevant to this daemon. Nothing here required or
implies a code change by itself — it's cross-checks and open questions for whoever
picks up front-panel/LED work next.

**License note:** none of Korg's firmware, disassembly, or artwork is reproduced
here — only short factual UI-string labels and structural findings, same posture as
the rest of this repo's documentation.

---

## 1. `/dev/fb1`'s 8bpp-indexed model is now confirmed from the other end

The panel board is not a dumb display — it's a separate TI OMAP-L1x SoC running its
own firmware, with its own LCD controller (`clcdc.cpp`) and touch driver
(`ctouchpanel.cpp`). Its firmware embeds a **byte-for-byte-recoverable 256×RGB
palette table**, and 229 of its 256 entries match `source/palette_data.h` exactly.
This confirms the wire model this daemon already assumes: the host renders an 8bpp
palette-indexed 800×600 framebuffer, `OmapNKS4Module.ko` ships the *indices* (not
RGB) over USB, and the panel board applies (mostly) this same fixed palette on its
own LCD controller. No action needed — this is a validation of the existing design,
not a gap.

The 27 mismatched palette entries (indices ~125-126 and ~232-255) are exactly where
the firmware's own static table is placeholder black — i.e. those slots are
dynamically repainted at runtime (gradients/photos) rather than fixed at boot. If
`palette_data.h` is ever re-captured, don't be surprised if that same range drifts
between captures — it's content-dependent, not a capture bug.

## 2. Authoritative switch/LED name table (73 entries) — worth a naming diff

Korg's own hidden factory self-test menu enumerates every physical switch/LED by
index with an exact name string (full table in the kronosology doc linked above).
Cross-reading it against `btn_table[]` in `source/screenremote.c`:

- **Mode-select button 7 (`SETLIST` in this daemon) is internally named `"Live"`** in
  Korg's own firmware, not "Setlist". Purely a naming curiosity — the physical
  button and daemon behavior are unaffected — but explains why you won't find
  "Setlist" anywhere in Korg's own strings if you go looking.
- Firmware confirms **bi-color LEDs**, structurally, for the two buttons this
  matters most for:
  - Sampling Start/Stop → separate `"Sampling Green"` / `"Sampling Rec"` LED entries
  - Seq Start/Stop → separate `"Seq Green"` / `"Seq Red"` LED entries

  This daemon has no LED-state introspection today (`STATE`/`SYSINFO` don't expose
  it), so this is forward-looking: if LED read-back is ever added, these two buttons
  need a 2-bit (or tri-state incl. off) LED representation, not a boolean.
- **Possible gap, unconfirmed:** the firmware's table has five separate LED entries
  for KARMA module selection (`KARMA Module All/A/B/C/D`), while `btn_table[]` has
  only one injectable code, `MODULE_CONTROL` (47). This is consistent with either
  (a) one physical "cycle" button driving 5 indicator LEDs (matches the *separate*
  string `"KARMA Module Ctr"` found in the firmware's general control-name list —
  more likely reading), or (b) four discrete per-module select buttons the daemon
  can't currently inject. Similarly, `"DrumTrack Linked"` appears as its own LED
  entry alongside `"DrumTrack On/Off"`, and only `DRUM_TRACK` (50) exists in
  `btn_table[]`. **Not verified either way — before adding codes on spec, confirm
  on real hardware** (e.g. re-run the NKS4 test/calibration capture used to build
  the existing table, watching specifically for whether pressing the module-control
  area ever yields more than one distinct scan code).

## 3. Not investigated further, flagged for whoever wants it

- `../CryptoAt88.cpp` is a source file in the panel firmware's string table —
  i.e. the panel board may talk to the AT88 crypto chip independently of the host's
  `OA.ko`/`GetPubIdMod.ko` path. Only one weak xref was found (an assertion string),
  not chased further. Irrelevant to this daemon's day-to-day operation, but a lead
  for the AT88/`pairFact3` research thread in `kronosology`.
- The panel firmware embeds its own boot splash (KORG/KRONOS logo, 800×600, same
  8bpp indexed format) — confirms the 800×600 canvas size end-to-end, nothing more
  actionable for this daemon.
