# EVA CModeManager probe (`eva_mode_peek.ko`)

Status: **built, live-calibrated, FULLY CONFIRMED, and DEPLOYED to screenremote.c 2026-07-17 - live in production.**

## 2026-07-17 production integration

`eva_mode_module/eva_mode.c` is the production counterpart of
`eva_mode_peek.c` - same calibrated pointer chain and lowest-PID fix, trimmed
(no diagnostic hexdump) and reporting a single-line `/proc/.eva_mode` for
`screenremote.c` to parse. It's now embedded into the daemon (same
`xxd -i`-embedded-buffer + `init_module(2)` pattern as `vkbd.ko`/
`nks4_inject.ko`, loaded early at startup, no dependency on OA.ko or Eva
being up yet - see `screenremote.c`'s `main()`) and is the **primary**
source for `STATE`/`SYSINFO`/`MODE_DETAIL`'s `MODE`/`EDITCTX` fields
(`get_mode_state()` in `screenremote.c`), with the existing pixel-based
`detect_ui_mode()`/`detect_program_edit_context()` retained as a fallback
for when `eva_mode.ko` isn't loaded or hasn't resolved yet (early boot).
`eva_mode.ko` reports raw `SYS_MODE`/`EDITCTX_RAW`; the translation to this
daemon's public `MODE=1..7`/`EDITCTX=0..2` wire numbering
(`g_eva_sysmode_to_pub[]`) lives in `screenremote.c` itself, not the kernel
module, so a mapping fix only needs a daemon rebuild.

Also added `MODE_DETAIL`, a new read-only control-port command (richer
counterpart to `STATE`, same relationship as `PADMAP_STATE` to `PADMAP_*`)
reporting `SOURCE=eva|pixel`, `EDITSLOT`, and whether `eva_mode.ko` is
loaded/resolved - see `docs/api.md`.

**Live-deployed and verified 2026-07-17**: backed up the running
1.11.2 binary (`screenremote.1.11.2-pre-evamode.bak`, matching this
device's existing versioned-`.bak` convention), swapped in the new build
(no client was connected at the time), confirmed `eva_mode loaded` in the
startup log, then queried `STATE`/`MODE_DETAIL` over the control port and
cross-checked the result against an independently-computed pixel score at
the same moment - both agreed exactly (`MODE=3` Program, 1.000 pixel match,
`SOURCE=eva EVA_RESOLVED=1`). No reboot occurred during the swap
(`/proc/uptime` monotonic throughout). The diagnostic `eva_mode_peek.ko`
was `rmmod`'d afterward, now fully superseded by the production module.

**Reviewed by Opus 2026-07-17** (module unload safety across various exit
paths, plus general code review): no fix needed for unload safety - this
kernel's `proc_dir_entry` rundown (`pde_users`/`pde_unload_completion` in
`fs/proc/generic.c`, traced directly against `/home/build/linux-kronos`)
makes `remove_proc_entry()` block until any in-flight `/proc/.eva_mode`
read completes, and `flush_scheduled_work()` guarantees the deferred
`create_proc_entry()` has resolved one way or the other before
`module_exit()` checks it - both `rmmod` and `rmmod -f` are safe with no
module-refcount management needed (this kernel's `proc_dir_entry` doesn't
even have an `owner` field; it was removed in favor of this exact
mechanism). No leaks found in the mm/page/RCU reference handling. One real
finding applied: `eva_mode_read()` (`screenremote.c`) validated `SYS_MODE`
against `g_eva_sysmode_to_pub[]`'s bounds but passed `EDITCTX_RAW` through
unchecked - fixed to reject (fall back to pixel detection for *both*
fields, not just pass through a half-trusted `EDITCTX`) if it's outside
`EDITCTX_NONE..EDITCTX_SEQUENCE`, matching how `SYS_MODE` was already
handled. Redeployed and reverified live (`screenremote.1.11.2-pre-editctx-
clamp.bak`) - `MODE_DETAIL` still reports `SOURCE=eva` correctly, no
reboot.

## 2026-07-17 calibration session - fully confirmed

Ran an interactive session with the module loaded: for each mode/edit-context
state, the user navigated the physical unit into it and confirmed verbally,
and each reading was cross-checked against independently-computed pixel
ground truth (same method as the 2026-07-16 test below) at the same moment.
Every value below was directly observed at least once (most were also
cross-confirmed via `PREV_MODE` matching the previously-visited mode):

```
SYS_MODE:    0=Program  1=Combi  2=Global  3=Disk  4=Sequence  5=Sampling  6=Setlist
EDITCTX_RAW: 0=none  1=Program-edit-from-Combi  2=Program-edit-from-Sequence
```

This ordinal mapping is its own, apparently arbitrary C++ enum declaration
order - it matches neither the SysEx wire numbering used elsewhere in this
project (`KronosSysEx.cs`'s `SysExModeData`: 0=Combi, 2=Program, 4=Sequencer,
6=Sampling, 7=Global, 8=Disk, 9=Setlist) nor the client's own 1-indexed
`Mode` enum (`Core/Mode.cs`: 1=Setlist..7=Disk). Don't assume either of those
numberings applies to `ESysMode`/`eSTGEditInContextType`.

The 2026-07-16 mismatch documented below turned out to be exactly what it
looked like it might be: the read mechanism (pointer chain, offsets) was
correct and reading real live data the whole session - the only thing wrong
was the *guessed* ordinal-to-name mapping. `EDITCTX_RAW=2` for
Program-edit-from-Sequence, the one value that was purely inferred by
decompile symmetry with no confirmed call site, turned out to be exactly
right - it was independently observed twice more in this session (both
times the user described exactly that state) in addition to the two
observations from 2026-07-16, now with direct ground-truth pixel
confirmation (1.000 match on the Program banner + 1.000 match on the
Combi-edit indicator region for the `EDITCTX_RAW=1` case specifically).

## 2026-07-17 Help/Compare exploration - clean negative result

None of the 6 calibrated fields obviously correspond to the Help overlay or
the Compare toggle, and `CModeManager`'s true size is clearly bigger than
the ~44 bytes the ctor decompile suggested (valid data was already being
read at +0x34). Rather than guess more named offsets, added a raw hex dump
of `CModeManager+0x00..+0x4F` (`DUMP=` field, see `dump_eva_bytes()`) to
diff before/after toggling each, on the theory that whatever byte tracks
Help/Compare would show up even without knowing its name in advance.

Result: the dump was **byte-for-byte identical** with Help on vs. off, and
separately byte-for-byte identical with Compare on vs. off (baseline
captured in Program mode specifically, since Compare needs an edit
target). Clean negative - Help and Compare state live in some other object
in Eva entirely, unrelated to mode/edit-context navigation. This is
consistent with the client's own `Detection/ModeDetector.cs`, which already
treats Help as a wholly separate pixel-region check (`IsHelpActive()`, its
own reference bitmap/row-band) uncoupled from mode detection - Eva's own
internal structure apparently mirrors that separation. Not pursued further;
finding the actual Help/Compare state object would need a much broader
memory hunt, out of scope for mode detection.

## 2026-07-17 gotcha: transient same-named process

Mid-session, one read returned `RESOLVED=0 STAGE=read_sm_pommi EVA_PID=32227
RC=-14` - a completely different PID from the stable `1380` used all
session. By the time it was checked, `/proc/32227` had already exited
(empty `cmdline`/`status`); the real Eva (`1380`) was confirmed still alive
and unaffected the whole time. `find_eva_mm()`'s "first `comm==eva_comm`
match, walking from `current`" is a plain linear search with no
tie-breaking - it got unlucky and briefly matched some other short-lived
process that happened to also report `comm=="Eva"` (possibly a forked
child before it renamed/exec'd, or exited). A retry a few seconds later
resolved normally against `1380` again.

**Fixed 2026-07-17.** Tried to reproduce it directly first: ran a tight
`/proc` polling watcher on-device (no network round trips in the loop) while
the user repeatedly opened Help and Compare - it caught nothing in two
90-second windows, so it isn't reliably triggered by either. A parallel
decompile search (see below) found the real explanation: several of Eva's
own `fork()`+`execl()` helpers (`CFileOperation::TimeStamp`,
`IsOverCurrentCondition`, `MountUSBDevice`, `set_date` - all periodic
USB/disk housekeeping, unrelated to mode-switching or Help/Compare) never
call `_exit()`/`exit()` if the `execl()` fails, so a failed exec leaves a
second live `comm=="Eva"` process running ordinary Eva code until it exits
on its own - a real bug in Eva itself, just not one worth chasing further
here. Since the genuine long-lived UI process was PID-stable at `1380`
across both sessions, and any such transient child necessarily forks
*after* it (so always has a strictly higher PID), `find_eva_mm()` now scans
all `comm==eva_comm` matches and picks the **minimum PID**, rather than the
first one the RCU list walk happens to reach - reliably selects the real
Eva even if a transient collides. See `find_eva_mm()` in
`eva_mode_peek.c`.

### Why the transient process wasn't a mode/Compare/Help fork after all

Static search of the full Eva decompile (`EVA_Decomp/eva_export`, ~38k
functions) for `fork()`/`vfork()`/`clone()`/`system()`/`popen()`/
`pthread_create` found **no fork calls reachable from `ChangeMode`,
`ChangePage`, `SetEditInContext`, or the Compare family at all** - all 14
real `fork()` call sites are disk/USB utility helpers (`mountvfat`,
`detectusbocc`, `mv`, `touch`, `fsck`, etc.), none of them mode- or
Help/Compare-related. The most plausible trigger is
`IsOverCurrentCondition()`, driven by periodic USB-overcurrent poll/notify
timers - i.e. the timing coincidence with the mode switch during the
original observation was very likely just that, a coincidence, not
causation.

No reboot occurred during either session (`/proc/uptime` monotonic
throughout). The module was left loaded at the end of this session for
convenience mid-investigation - `rmmod eva_mode_peek` on `192.168.100.15`
when done with it, it's diagnostic-only and not meant to run long-term.

## 2026-07-16 live test result (superseded above - kept for the debugging trail)

## 2026-07-16 live test result

Deployed to the live Kronos (192.168.100.15) and loaded successfully after
one fix (see "Symbol export gotcha" below). It resolves and reads live,
stable-looking heap pointers - but the values don't match reality.

Cross-checked against independently-computed ground truth: queried the
already-running (pre-existing, unmodified) `screenremote` daemon's `REGION`/
`PALETTE` commands for the actual live top-left banner and (696,39)
edit-context-indicator pixels, and scored them locally against the exact
same reference data `screenremote.c`'s `detect_ui_mode()`/
`detect_program_edit_context()` uses (`mode_detect_refs.h`). Result: the
Kronos was definitively in plain Program mode (1.000 match, i.e. every
single reference pixel matched) with no edit-context overlay active (0.547
on the Combi-edit indicator, well under its 0.98 activation threshold).

`eva_mode_peek.ko` reported, at the same moment: `SYS_MODE=0`,
`PREV_MODE=4`, `EDITCTX_RAW=2`. `SYS_MODE=0` doesn't map to Program under
the SysEx wire numbering (0=Combi) or a plausible 0-indexed variant of the
client's own `Mode` enum (0=Setlist) - no ordering tried makes 0 mean
Program. `EDITCTX_RAW=2` directly contradicts the confirmed "no edit-context
active" ground truth (should read 0). `CMMI_PTR=0x0ee46ce0` /
`MODEMGR_PTR=0x0ee46ed8` look like real, plausible heap addresses (not
NULL/garbage), so the module isn't reading nothing - either the specific
field offsets, or the `sm_poMMI -> CMMI::modeManager` structural assumption
itself, need re-deriving from the decompile. **Do not trust this module's
output until that's resolved.** No reboot occurred during this test
(`/proc/uptime` increased monotonically 4988s -> 5235s -> 5416s across the
session); the module was `rmmod`'d afterward, leaving the device clean.

### Symbol export gotcha (fixed)

The first build failed to `insmod` at all: `Unknown symbol
__put_task_struct`. The original design held a `get_task_struct()`
reference across the read sequence and released it with
`put_task_struct()`, whose slow path calls `__put_task_struct()`. Grepping
`/home/build/linux-kronos/kernel/fork.c` confirmed neither
`__put_task_struct()` nor `tasklist_lock` carry an `EXPORT_SYMBOL` - both
are visible in `/proc/kallsyms` (so their *addresses* are knowable) but not
linkable by an out-of-tree module, which is exactly what "Unknown symbol"
means. Fixed by walking the `tasks` list under `rcu_read_lock()` alone
(needs no exported symbol - `kernel/exit.c`'s `__unhash_process()` already
uses `list_del_rcu()` and defers the actual free past an RCU grace period
via `call_rcu()`, so this is safe) and taking `get_task_mm()`'s own
reference on the `mm_struct` instead of ever holding a `task_struct`
reference outside the RCU section. See `eva_mode_peek.c`'s
`find_eva_mm()` for the full writeup.

## What this is

`screenremote.c`'s `detect_ui_mode()`/`detect_program_edit_context()` (added to
report `MODE=`/`EDITCTX=` on `STATE`/`SYSINFO`) work by fingerprinting specific
pixel regions of `/dev/fb1` against reference bitmaps ported from the C#
client's `Detection/ModeDetector.cs` / `Detection/CombiProgramEditDetector.cs`.
That works today, but it's inherently approximate (color-tolerance thresholds,
85-98% match fractions) and blind to any UI state the client never captured a
reference bitmap for - notably the Sequencer-side analog of
Program-Edit-from-Combi, which `KronosScreenRemote` never implemented either.

While researching that feature, tracing `EVA_Decomp` turned up a much more
direct source of truth: Eva's own `CModeManager` object, living at a fixed,
non-relocated address in Eva's process memory (Eva is a plain `ET_EXEC` x86
binary, image base `0x08048000`, not PIE/ASLR'd - see
`kronosology/docs/modules/Eva.md`). The field offsets below are now fully
confirmed (see the 2026-07-17 calibration session below): reading a handful
of ints out of Eva's memory replaces pixel-fingerprinting entirely - no PNG
references, no thresholds, and it covers the Sequence edit-context case
pixel detection never could.

`eva_mode_peek_module/eva_mode_peek.c` is a **one-shot, read-only diagnostic**
kernel module (same bar as `shm_peek_module/shm_peek.c` and
`chord_probe_module/chord_probe.c` - no hooking, no patching) that resolves
Eva's `task_struct` by scanning the process list for `comm=="Eva"`, then reads
through its memory via `get_user_pages()` (the same mechanism `/proc/pid/mem`
uses) to dump:

```
sm_poMMI (VA 0x0ae431b0, in Eva's own address space)
  -> +0x04  CMMI::modeManager        (CModeManager*)
       -> +0x04  ESysMode              current top-level mode
       -> +0x20  ESysMode              target mode mid-transition
       -> +0x24  short                 current sub-page id
       -> +0x28  ESysMode              previous mode
       -> +0x30  eSTGEditInContextType 0=none 1=Combi 2=Sequence (all 3 confirmed 2026-07-17)
       -> +0x34  int                   timbre/track slot being edited, -1=none
```

## Confirmed vs. inferred

- **Confirmed (static + live)**: `sm_poMMI`/`CMMI::modeManager` chain exists
  and matches this shape, from `CModeManager`'s own constructor
  (`functions/CModeManager@08965310.c`) plus `ChangePage@08965620.c`,
  `SetEditInContext@08966740.c`, and
  `IsOnTimbreProgramEditInContext@08966580.c`. `eSTGEditInContextType==1`
  (Combi) is confirmed by a real call site: `CombiMsgHandler`
  (`functions/CombiMsgHandler@08919360.c:208`) calls
  `IsOnTimbreProgramEditInContext` in exactly the Combi-edit scenario.
- **Confirmed (live only, 2026-07-17)**: the full `ESysMode` ordinal mapping
  (all 7 values) and the full `eSTGEditInContextType` mapping (all 3
  values, including the Sequence case that had no confirmed call site
  statically) - see the calibration session above. That Eva is genuinely
  non-PIE/ASLR-off on real hardware and `0x0ae431b0` is the live address
  every boot is also now implicitly confirmed (every read across two
  sessions and a module reload in between resolved correctly).
- **Investigated and NOT tracked here**: Help overlay state and Compare
  toggle state - confirmed by byte-for-byte identical dumps of
  `CModeManager+0x00..+0x4F` with each on vs. off (2026-07-17). They live in
  some other Eva object; out of scope for mode detection.
- **Fixed 2026-07-17**: `find_eva_mm()`'s task lookup could transiently match
  a short-lived unrelated process that happens to share `comm=="Eva"`
  (observed once live) - now picks the minimum PID among all matches
  instead of the first one found, since the real Eva is always the lowest.
  See "transient same-named process" note above.

## Build

```bash
cd eva_mode_peek_module
make -f Makefile.module        # KDIR defaults to /home/build/linux-kronos
```
Builds clean as of 2026-07-16. `nm -u eva_mode_peek.ko` shows only standard
mm/task-list primitives as unresolved externs (`get_task_mm`,
`get_user_pages`, `tasklist_lock`, `down_read`/`up_read`, `kmap`/`kunmap`,
`put_page`, `get_task_struct`/`put_task_struct`, `mmput`) - no unexported or
guessed symbols. This kernel tree has no `Module.symvers`, so modpost could
not itself verify these are actually exported by the real running kernel;
that's only checked at insmod time.

## Calibration procedure (run 2026-07-17 - kept for future reference/re-runs)

1. `scp -O eva_mode_peek.ko root@192.168.100.15:/korg/rw/`
2. `insmod /korg/rw/eva_mode_peek.ko` (default `eva_comm=Eva
   sm_pommi_addr=0x0ae431b0`)
3. Confirm it loads and resolves: `cat /proc/.eva_mode_peek` should show
   `RESOLVED=1` with a plausible `EVA_PID`. If `RESOLVED=0`, `STAGE=` says
   which pointer hop failed - `find_task` means no process named `Eva` was
   found (check the real comm via `cat /proc/<pid>/comm` for Eva's actual
   PID if `ps`/`pidof` aren't available; also see the "transient same-named
   process" gotcha above - just retry), `read_sm_pommi`/`read_modemgr_ptr`
   means the fixed address or `+0x04` hop is wrong for this build.
4. Press each mode-select button and record `SYS_MODE` each time. **Done -
   see the full table above.**
5. Get a Program into "edit from Combi" and confirm `EDITCTX_RAW` reads 1.
   **Done.**
6. Get a Program into "edit from Sequence" and confirm `EDITCTX_RAW` reads
   2. **Done.**
7. `rmmod eva_mode_peek` when done - it's read-only/diagnostic, not meant to
   stay loaded long-term.

## Once calibrated (now true - next step, not yet done)

The field layout and full ordinal mapping are confirmed. The natural next
step is folding this into `screenremote.c` as the mode source (replacing or
supplementing the pixel-tolerance/85-98%-threshold approximation of
`detect_ui_mode()`/`detect_program_edit_context()`), which would also let
`EDITCTX_SEQUENCE` be reported for the first time -
`mode_detect_refs.h`'s `g_seq_edit_ref` currently has no reference bitmap
and can't detect that case at all via pixels. Before that happens:

- Fix the transient-same-name-process gap in `find_eva_mm()` (see above) -
  more important once this runs unattended/continuously in the daemon
  rather than being read interactively.
- Decide the integration shape: read via `get_user_pages()` from
  `screenremote.c` itself (userspace, no kernel module - simplest, but
  screenremote runs as a normal process and would need the same mm-reading
  capability, likely via `/proc/<eva_pid>/mem` + ptrace instead, since
  `get_user_pages()` isn't reachable from userspace), vs. keeping
  `eva_mode_peek.ko`-style kernel-side reads and exposing the result to
  `screenremote.c` via a `/proc` file it polls, vs. folding the read logic
  directly into a permanently-loaded kernel module. Not decided yet - ask
  before implementing, this changes the daemon's architecture, unlike the
  earlier pixel-detection addition which was self-contained.

Until that integration happens, `screenremote.c`'s screen-pixel detection
remains the deployed source of truth; `eva_mode_peek.ko` stays a loaded (or
easily reloadable) diagnostic.
