# KronosScreenRemoteDaemon — VM Test Environment

This document records the complete state of the QEMU virtual machine used for
testing `screenremote`. It covers the host, the QEMU invocation, the guest
kernel and modules, the network stack, the screenremote daemon, and every
finding about what works and what does not. Use this as a reference when
comparing against other environments or when setting the VM up fresh.

---

## 0. 2026-07-24 update — full OA.ko/Eva integration, RE work, boot chain fixes

Everything in sections 1-11 below predates this update and describes the
**older `kronos_vm` boot stub that never loaded OA.ko/Eva at all** (fakefb +
network + screenremote only, `-drive`-only GRUB boot). As of this session,
`kronos_vm`'s `setup_vm.sh`/`overlay/sbin/loadoa`/`run_vm.sh` boot the full
chain: RTAI substitute → `kronosology/reconstructed/OA/OA.ko` (with its
hardware-stub siblings) → `reconstructed/Eva/Eva` → screenremote. Treat
anything below that conflicts with this section as historical/superseded;
re-verify against the current `kronos_vm/` scripts before trusting specifics.

**New OA.ko reconstruction work landed** (all four independently RE'd from
raw `objdump -dr` against the real `OA.ko`, MD5 `955636c2b11a70a1dbecefaaa7bd4f80`,
cross-checked against this daemon's own already-ground-truthed real-hardware
findings in `nks4_inject.c`/`midi_bridge.c`):
- `CSTGFrontPanel::HandleSwitchEvent/HandleTouchPanel/HandleRotary/HandleAnalogController`
  + `ShortInvertNkS4AnalogValue` — `src/engine/front_panel_handlers.cpp`.
- `RT_chord_trigger` (KARMA pad-chord trigger) — `src/engine/karma_chord_trigger.cpp`.
- MIDI IN: `CSTGMidiInPortGeneric::Receive` (the real function behind
  `MidiInPortGeneric7Receive`) + `CSTGMidiInPort` fields — `src/engine/midi_in_port.cpp`.
- MIDI OUT: `CSTGMidiPortManager::RegisterMidiOutPort` + `CSTGMidiOutPort::Activate`
  (queue-tap plumbing `midi_bridge.ko` needs) — `src/engine/midi_out_port.cpp`.
- 3 VM-testing-only accessor exports (`CSTGFrontPanel_GetInstanceForTest`,
  `CSTGMidiPortManager_Get{In,Out}PortsArrayForTest`) — `src/engine/vm_test_accessors.cpp`.
  These do **not** exist on real hardware, so they're always safe/no-op there.

**Daemon-side changes to support the VM** (all strictly opt-in, zero
real-hardware behavior change — new params default to 0/unset):
- `nks4_inject_module/nks4_inject.c` — new optional param `fn_sinstance_get`.
  When set, calls it directly for `CSTGFrontPanel::sInstance` instead of the
  real-hardware-only `SINSTANCE_REL_OFFSET` byte-offset trick (which is
  calibrated to the real OA.ko's specific GCC-4.5.0 codegen and does not
  transfer to this from-scratch reconstruction's own compiled layout).
- `midi_module/midi_bridge.c` — new optional params `fn_inports_get`/
  `fn_outports_get`, same idea, bypassing the real-hardware-only
  `RegisterMidiInPort`/`RegisterMidiOutPort` byte-pattern scan.
- `source/screenremote.c` — `resolve_kallsyms()`/`resolve_nks4_kallsyms()`
  now also look up the 3 `*ForTest` symbol names above and pass their
  addresses through to the new params. On real hardware these symbols never
  appear in `/proc/kallsyms`, so the lookup returns 0 and behavior is
  unchanged.

**`kronos_vm` boot-chain bugs found and fixed this session** (all in
`/home/share/kronos_vm/`, not this repo):
1. `/etc/OA.si` (the real, untouched stock rootfs's own sysinit script)
   hardcodes `mount ... /dev/sda5 /korg/ro` / `/dev/sda6 /korg/rw` — but this
   kernel's classic `ide-gd` driver (not libata/`ata_piix` SCSI translation)
   enumerates the `-drive if=ide` disk as `/dev/hda*`. Both mounts silently
   failed, leaving `/korg/rw` (and everything under it — `oa_recon/`, `Eva`,
   `screenremote/`) empty for the entire boot. Fixed with a corrective
   re-mount in `overlay/etc/vm_init.sh` (runs before `OA.rc`/`loadoa`).
2. `setup_vm.sh`'s generated GRUB2 `grub.cfg` boots this exact kernel+rootfs
   but hangs **deterministically and silently forever**, well before the
   "Linux version" printk, at a fixed address in the kernel's own early
   `.text` (confirmed via QEMU monitor `info registers`/`x/5i $eip` —
   identical EIP every time, ruling out timing races). Root cause not fully
   pinned down (likely a GRUB2 boot_params/E820/MP-table construction
   difference this kernel's early init doesn't expect); worked around by
   having `run_vm.sh` boot via QEMU's own `-kernel`/`-append` loader instead
   of the disk's GRUB2/MBR, which reaches "Linux version..." immediately.
3. The full kernel cmdline matters and is fragile: `vga=0x0303 fbcon=map:0
   console=tty0 vmalloc=512M` must all be present. Dropping `vga=`/`fbcon=`/
   `console=tty0` (an earlier revision of `run_vm.sh` did this, wrongly
   theorizing they caused bug #2 above) causes a **different**, later hang:
   boot proceeds fine through "Linux version"/VFS mount/the kernel's own
   compiled-in RTAI I-pipe registration ("I-pipe: Domain RTAI registered"/
   "RTAI[hal]: mounted" — unconditional kernel printks, unrelated to
   `loadoa`'s own choice of RTAI `.ko`), then parks forever in `HLT` with
   interrupts enabled but never firing again.
4. Even with the correct full cmdline, RTAI I-pipe calibration under
   QEMU/TCG shows **genuine run-to-run nondeterminism** — the identical
   config sometimes clears this point in under 2 minutes, sometimes stalls
   indefinitely. This matches this project's own long-documented "severe
   run-to-run variance" for RTAI-under-TCG timing (see
   `kronosvm_dedicated_sandbox_87` and `oa_ko_rtai_virtualization_policy`
   auto-memory files) — not a new bug, just now observed on this specific
   boot chain too. **If a boot seems stuck here, do not `pkill` it** —
   repeated hard-kills mid-boot corrupt the disk image's ext2 filesystems
   (`EXT2-fs error: deleted inode referenced`); recoverable via
   `guestfish --rw -a kronos.img -- run : e2fsck /dev/sdaN forceall:true`,
   but better to just let a stuck boot keep running (or retry from a fresh
   image) than interrupt it.
   **CORRECTED below in section 0b — this "nondeterminism" framing was
   wrong. The real cause was a missing-device-nodes bug that made
   `FAST_RTAI` silently fail to take effect, so genuine `rtai_hal.ko` was
   loading (and stalling) on every boot regardless of RTAI's own timing.
   Root-caused and fixed same day.**

**Status at end of this session**: all reconstruction/wiring/build work
(RE'd OA.ko functions, daemon kernel-module opt-in fallback paths, daemon
binary rebuild, boot-chain fixes 1-3 above) is done and independently
verified (OA.ko rebuilds clean with all new symbols present via `nm`, full
OA host test suite green — 101 binaries / 4054+ checks / zero failures,
daemon builds clean). The *specific* end-to-end boot-to-screenremote
milestone (confirming the TCP handshake against the live VM) was not
reached by session end due to issue #4's nondeterminism, not any known
remaining code defect — retry `kronos_vm/run_vm.sh` (ideally on the
dedicated `kronosvm` sandbox, 192.168.3.87, not a contended host) and
watch `boot_console.log` past the "RTAI\[hal\]: mounted" point.

## 0b. 2026-07-24 update (same day, continued) — RTAI hang root-caused and fixed; boot now clears RTAI entirely; new frontier found

**The "genuine RTAI/TCG nondeterminism" framing in point #4 above was
wrong.** Root-caused and fixed this session by digging into the actual
RTAI 3.8.1 source (available locally at
`/home/share/Korg_Kernel_src/rtai-3.8.1.tar.bz2`) and cross-referencing
against a live QEMU-monitor EIP-sampling pass (`-monitor telnet:...,server`,
now added to `run_vm.sh` permanently) and postmortem `guestfish` disk
inspection.

**Root cause**: `$ROOTFS` (`RestoreDVD_SystemMNT/mnt`, the extracted real
Kronos install image) lives on a CIFS network share. CIFS/SMB cannot store
Unix device/block special files — the source rootfs's own `/dev` entries
were silently flattened to empty **regular files** at extraction time
(confirmed: `file(1)` reports "empty", not "character special", for
`console`/`null`/etc. in `$ROOTFS/dev` itself). `setup_vm.sh` never
recreated real device nodes, so the built VM image's `/dev/console`,
`/dev/ttyS0`, `/dev/ttyS1`, `/dev/kmsg`, `/dev/null`, and **every
`/dev/hda*` block device** were also just regular files (`-rw-`, not
`crw-`/`brw-` per `guestfish ll`).

**Consequence chain**:
1. The kernel can mount `root=/dev/hda2` via its own early boot-time
   name-to-`dev_t` resolution with zero `/dev` entries required — so root
   mounts fine and boot looks normal at first.
2. But `overlay/etc/vm_init.sh`'s own `/korg/ro`/`/korg/rw` mount-fix
   (`mount -t ext2 /dev/hda5 /korg/ro`, `mount -t ext3 /dev/hda6 /korg/rw`)
   is a *userspace* `mount` call, which needs a real device node to open —
   with none present, both mounts silently failed with ENOENT on **every
   single boot**, confirmed live via `[vm_init] korg/ro mounted: no` /
   `korg/rw mounted: no`.
3. `/korg/rw` therefore stayed an empty, unmounted directory for the rest
   of boot — so `overlay/sbin/loadoa`'s own
   `[ -f /korg/rw/oa_recon/FAST_RTAI ]` check always saw nothing there and
   always fell through to the `else` branch, loading **real**
   `rtai_hal.ko`/`rtai_sched.ko`/etc. instead of the already-proven-working
   `RTAIVirtualDriver.ko` substitute — confirmed live via a
   `CONSOLE-MARKER: loadoa FAST_RTAI check: ABSENT` line.
4. It was *this* real-RTAI fallback that stalled under QEMU/TCG on most
   attempts (observed anywhere from ~5 to ~10+ minutes before a manual
   kill), not a defect in RTAI's own calibration. Traced the exact hang
   point into RTAI 3.8.1's `base/sched/sched.c` — the calibration printk
   immediately before the stall point (`RTAI[sched]: timer setup = 999 ns,
   resched latency = 2943 ns.`, line ~3078) prints **plausible, non-garbage
   numbers**, proving RTAI's own timing measurement had already completed
   successfully; the actual hang was in loading the subsequent silent
   companion modules (`rtai_sem.ko`/`rtai_ndbg.ko`/`rtai_fifos.ko`, none of
   which print an init banner on success).

**Diagnostic methods that turned out to be unreliable** (document these so
a future session doesn't waste time rediscovering them):
- `kmsg()`-style `echo ... > /dev/kmsg` logging was silently going into the
  same bogus regular-file `/dev/kmsg`, not the kernel's real printk ring
  buffer — `printk.devkmsg=on` on the kernel cmdline was a red herring the
  whole time; there was no real kmsg device to forward from.
- The file-based `/root/checkpoint.log` diagnostic (written with an
  explicit `sync` after every step) still never survived a hard
  `pkill -9`/`kill -9` of the QEMU process even once — most likely QEMU's
  own drive write-back caching not committing to the host-side `kronos.img`
  file before the process died, independent of the guest's own `sync`
  syscall succeeding.
- The technique that actually worked: writing unique marker strings
  directly to `/dev/ttyS0` at the top of `OA.si`/`vm_init.sh`/`loadoa` (and
  at loadoa's `FAST_RTAI` check specifically), then reading back the
  (at-the-time-bogus) regular file's content via postmortem `guestfish cat`
  after a kill — this revealed the true execution state even before the
  device-node bug itself was found and fixed. Once real device nodes exist,
  these markers appear live in `boot_console.log` instead of needing
  postmortem recovery.

**Fix**: `setup_vm.sh` now explicitly creates real device nodes via
guestfish's own `mknod-c`/`mknod-b`, right after the root filesystem
`copy-in` step:
- Char: `console` (5,1), `null` (1,3), `zero` (1,5), `random` (1,8),
  `urandom` (1,9), `kmsg` (1,11), `ttyS0` (4,64), `ttyS1` (4,65), `tty`
  (5,0), `tty0` (4,0), `tty1` (4,1), `ptmx` (5,2).
- Block: `hda`, `hda1`–`hda8` (major 3, minor 0–8 — standard Linux primary
  IDE master numbering, matching this kernel's `ide-gd` driver).

**Result, confirmed live and repeatable**: `FAST_RTAI` now correctly reads
as present; `loadoa` loads `RTAIVirtualDriver.ko`/`STGEnabler.ko`/
`STGGmp.ko` cleanly — **zero real `RTAI[hal]`/`RTAI[sched]` console lines
appear at all**, the entire RTAI stage is sidestepped exactly as originally
designed, no hang. This is the furthest `kronos_vm` has ever booted.
Also fixes the previously-mysterious "Warning: unable to open an initial
console." boot message (the kernel's own attempt to open a real
`/dev/console`, which now exists).

**Items 1-2 below (`OA.ko` unresolved symbols, Eva) were FIXED same day —
see the new section 0c below for the fix and confirmed live result.** Item
3 (post-fakefb stall) remains the current, sole, still-open blocker.

1. ~~`OA.ko` insmod now fails on unresolved symbols: `__fixsfsi`/
   `__floatsisf`/`__mulsf3` (libgcc soft-float helpers pulled in by
   floating-point code somewhere in the module) plus four C++-mangled
   symbols — `CSTGControllerRTData::SendKarmaCCToKG`,
   `CSTGMidiInPort::ReceiveSysEx`, `CSTGControllerInfo::ButtonPressHandler`,
   `CSTGControllerInfo::AnalogControllerHandler`.~~ **FIXED, see 0c.**
2. ~~`reconstructed/Eva/Eva` segfaults immediately on launch (`ip
   080496cf`) — expected/consequential, since Eva depends on a live OA.ko
   to read panel/mode state from.~~ **Not actually a bug — see 0c: with
   OA.ko now loading, Eva runs its complete boot path and exits cleanly on
   its own. It was never truly segfaulting from Eva's own logic; the
   earlier segfault was purely a downstream consequence of OA.ko failing
   to load at all.**
3. Boot reaches `fakefb: init called` (prints `fb_mem`/`info`/`fbops`
   addresses successfully) and then stalls — CPU time keeps climbing
   (confirmed via `systemctl status`'s own cgroup CPU accounting over
   multiple samples, i.e. **not** a frozen/halted process) but no further
   console output appears. This exact "post-fakefb stall" was **already
   independently found and explicitly flagged as a separate, unresolved,
   out-of-scope issue in a prior session** — see
   `kronosology/.claude/agent-memory/re-decompiler/rtai_virtual_driver_substitute.md`'s
   own "Live boot test result" section. Not introduced today; still open;
   now the sole remaining blocker to the full boot-to-screenremote
   milestone.

**Process-management note for future sessions**: launching the VM via
`ssh kronosvm "nohup ... & disown"` is vulnerable to a real, previously-
documented hazard (see `kronosology/.claude/agent-memory/re-decompiler/rtai_calibration_hang_diagnosis.md`
sec 10.214) — the backgrounded qemu process gets killed when the
*launching SSH session's own systemd login-scope* tears down, on a
timescale unrelated to how long the launch command itself took to return.
This recurred this session (a live, CPU-actively-climbing qemu process
vanished with zero crash/panic evidence in its own stdout/stderr).
**Fix**: launch via `systemd-run --unit=NAME --collect
--working-directory=DIR -- CMD` instead — runs under `system.slice`,
immune to session teardown, inspect with `systemctl status NAME`.

**Port-reachability ground truth, for anyone tempted to test with
`nc -zv`**: `nc -zv 127.0.0.1 21`/`7374` report "open" as soon as QEMU
itself starts, *regardless of guest boot state* — this is only QEMU's own
`hostfwd` listener accepting the TCP handshake on the host side, not proof
the guest's FTP/screenremote service is actually running yet. Always test
for a real application-level response (e.g. an actual `220 ...` FTP
banner, via `exec 3<>/dev/tcp/host/21; cat <&3` or similar) before treating
a port as genuinely reachable.

## 0c. 2026-07-24 update (same day, continued again) — OA.ko now loads clean; Eva runs its full boot path; only the pre-existing post-fakefb stall remains

**`OA.ko`'s 7 unresolved symbols, fixed** (all 4 were pre-existing
"deliberately deferred extern" declarations from prior reconstruction
passes, already explicitly documented as out-of-scope in `oa_global.h`/
`oa_engine.h`'s own header comments — not a regression introduced by
section 0's same-day work, just the first time anyone tried a live insmod
against a build that includes their callers):
- `__fixsfsi`/`__floatsisf`/`__mulsf3`: `front_panel_handlers.cpp` (new in
  section 0) does genuine plain-C float arithmetic (ADC-to-CC scaling in
  `HandleTouchPanel`) but was missing the per-file `CFLAGS_<obj>.o :=
  -mhard-float -msse2 -mfpmath=sse` Makefile override that every one of
  its sibling float-using files already has (this project's own
  established, extensively-precedented pattern for avoiding the kernel
  build's default `-msoft-float` — see `Makefile`'s own extensive comments
  on `engine_startup_bits.o`/`wave_sample_convert.o`/etc.). Simply missed
  when the file was added; one added `Makefile` line fixed it completely,
  no code changes.
- `CSTGControllerRTData::SendKarmaCCToKG`, `CSTGMidiInPort::ReceiveSysEx`,
  `CSTGControllerInfo::ButtonPressHandler`,
  `CSTGControllerInfo::AnalogControllerHandler`: given safe no-op stub
  bodies in `src/stub/bar2_stubs.cpp`, this project's own established,
  extensively-precedented location for exactly this situation ("deliberate
  minimal, safe stub bodies for every symbol OA.ko's own call chains
  reference that has NOT yet been individually reconstructed" — see that
  file's own header). None of these four are reachable from kronos_vm
  boot-testing anyway (no physical front panel to generate real button/
  analog/touch-panel events, no physical MIDI hardware to generate real
  SysEx traffic), so a no-op is safe and correct for this purpose; the
  real per-button action table / analog jump tables / KARMA-CC-to-DSP path
  remain a genuinely deferred, separate future reconstruction task if ever
  needed for something beyond VM boot-testing.

Rebuilt `OA.ko` verified clean: `nm` shows zero remaining references to
any of the 7 symbols, and the full OA host test suite still passes
(`make verify`: 4054 checks, 0 failures, exit code 0) — the fix is purely
additive (a Makefile flag + 4 stub function bodies), no existing behavior
touched.

**Confirmed live in kronos_vm, for the first time in this project's
history**:
```
OA_DEBUG_MARKER 15
OA_DEBUG_MARKER 16
OA_DEBUG_MARKER 17
OA: init_module succeeded, tsc_lo=00000000 tsc_hi=00000000
[loadoa] OA.ko: loaded OK
[loadoa] OA.ko: LOADED OK
```
And with a live OA.ko underneath it, `reconstructed/Eva/Eva` no longer
segfaults at all — `eva_stdout.log` shows a complete, clean run: connects
to OA's shared memory, loads stored settings, runs its full `Mains()`
sequence through every sub-module (`MMainPanelDriver`, `MMainHIDDriver`,
`MMainAlphaKeybCtrl`, `MMainLinuxDriver`, `MMainEditor`, `MMainPanel`,
`MMainBatchDiskMan`, `MMainESCommon/ESProg/ESEffect/ESCombi/ESGlobal/
ESMOSS/ESSampling/ESSetList/ESSong/ESDisk`), starts its init/timing
threads, then exits cleanly on its own (`Start closing` / `End closing`,
zero errors, zero segfault line in dmesg) — matching this project's own
previously-recorded `eva_reconstruction_project` "boots end-to-end" goal
exactly. The earlier apparent Eva "crash" was purely a downstream
consequence of `OA.ko` failing to load at all, not a bug in Eva itself.
`loadoa`'s own post-launch check was also updated (previously mislabeled
*any* Eva exit within 8s as "exited/crashed" — now distinguishes this
known-clean `End closing` exit from a real crash by checking
`eva_stdout.log`'s own last line).

**What's left**: the pre-existing "post-fakefb stall" (section 0b, item 3)
is now the **sole remaining blocker** to the full boot-to-screenremote-
TCP-handshake milestone — everything upstream of it (RTAI substitute,
AT88/KorgUsbAudio/OmapNKS4 virtual drivers, `OA.ko`, Eva) is now confirmed
working end-to-end.

---

## 0d. 2026-07-25 investigation — post-fakefb stall root-caused to a
first-time-exercised kernel console/VT lock interaction, NOT fakefb.ko's
own code and NOT OA.ko; genuinely fatal after ~12 minutes, not infinite

Reproduced live on `kronosvm` (fresh boot, `run_vm.sh` unmodified except a
per-host `BZIMAGE`/`kronos.img` path override needed because this sandbox's
own `/home/share` CIFS mount — see `kronos_share_migration` memory — was not
mounted; canonical `run_vm.sh` itself was not changed). QEMU monitor added
permanently to `run_vm.sh` in the prior session
(`-monitor telnet:0.0.0.0:4445,server,nowait`) made this investigation
possible.

**Exactly where it hangs**: `fakefb_init()`'s own code runs to completion —
`fb_mem=`/`info=`/`fbops@.../screen_base@...` all print correctly — and then
calls `register_framebuffer(info)`. That call **never returns**: the very
next `fakefb` printk (`"register_framebuffer returned %d"`) never appears,
in this run or any prior one. `fakefb.ko`'s own source
(`kronos_vm/fakefb/fakefb.c`) is confirmed correct up to that call; the hang
is inside vanilla kernel code `register_framebuffer()` transitively invokes,
specifically the framebuffer-console take-over chain:
`register_framebuffer()` → `fb_notifier_call_chain(FB_EVENT_FB_REGISTERED)`
→ `fbcon_event_notify()` → `fbcon_fb_registered()` → `fbcon_takeover()` →
`take_over_console()` → `bind_con_driver()` (`drivers/char/vt.c`). This is
architecturally significant: **fakefb is the first framebuffer this VM has
ever registered with a live VT console attached** (the pre-OA.ko boot stub
in sections 1-11 never ran together with the RTAI/OA.ko/Eva chain), so this
is the first time this exact kernel code path has ever executed in this
project.

**QEMU-monitor EIP sampling across all 4 vCPUs** (`cpu N` + `info registers`
+ `x/Ni $eip` over the unix/telnet monitor socket, repeated several times
seconds apart) during the stall showed:
- **CPU0, CPU1, CPU3**: all three pinned at the *identical* address
  (`0x40106c5e`) across every sample. Disassembly (against
  `kronos_vm/rtai_investigation/vmlinux.bin`, a full but **stripped**
  ELF — no `.symtab`, no System.map found anywhere on the DVD tree or the
  `linux-kronos` build tree either) is an unambiguous textbook i386 ticket
  spinlock contended-acquire path: `lock xadd` to take a ticket, then
  `pause` / re-read / `jmp` back until "now serving" == "my ticket". The
  lock word lives at a fixed `.bss` address (`0x405c5084`), i.e. some
  statically-allocated kernel lock, not a heap/module one.
- **CPU2**: at a *different*, also-fixed address (`0x4026ae2c`), inside the
  kernel's generic `__delay`/`delay_tsc` (the TSC-based busy-wait body of
  `udelay()`/`__const_udelay()` — confirmed by disassembling the containing
  routine, which does `rdtsc`, computes a cycle delta, and loops with
  `pause` until the requested count elapses, touching per-thread-info
  preempt-count fields via `esp & 0xffffe000`). CPU2's `ESP` sat in
  vmalloc-range memory (`0xde85xxxx`), i.e. a dynamically-allocated kernel
  stack (kernel thread or workqueue worker), not a statically-placed idle
  thread.

**Ruled out — OA.ko's own delay/retry code**: read every `udelay`/`msleep`
call site in `reconstructed/OA/src/` against this exact scenario.
`CSTGKeybedInterface_Startup()` (`keybed_init.cpp`) does retry 10 rounds ×
6 ports probing for the real hardware's W83627 Super-I/O chip (hence the
`OA_COMPORT_DBG port N: no W83627 Super I/O chip found` spam visible in
every boot log) — but `CSTGComPort::Initialize()`
(`comport_init.cpp`)'s failure path (`DetectChipAt()` fails for both
`0x2e`/`0x4e`, which is what always happens in this VM — no real chip) hits
`port_failed` with **zero delay calls**; the loop's own `udelay()` calls are
only reached on the (unreachable-here) ACK-received path. `OA_DEBUG_MARKER
15/16/17` and `"OA: init_module succeeded"` all printed in this run,
confirming `init_module()` genuinely returned before Eva/fakefb ever ran —
the stall is provably downstream of OA.ko's own code, not inside it.

**Best-supported mechanism (not fully proven to a named symbol, see
caveat below)**: cross-referencing `linux-kronos` source, `bind_con_driver()`
(`drivers/char/vt.c:2960`) calls `acquire_console_sem()` (a blocking, global
semaphore, not the fb-local `fb_info->lock` fakefb's own header comment
already documents) around `csw->con_startup()` and the
`printk("Console: switching ...")` calls — this is exactly the call chain
above. Separately, the actual `console=ttyS0,115200` backing driver
(`drivers/serial/8250.c` `serial8250_console_write()`) holds
`local_irq_save()` **and** a per-port spinlock for its *entire* flush,
transmitting every buffered character through `wait_for_xmitr()` (a
`udelay(1)`-based busy-poll per character) while the flushing CPU holds
`console_sem` for the whole operation. This is a textbook shape for one CPU
(draining buffered printk backlog through the slow serial path) to hold
`console_sem` while other CPUs block forever in `acquire_console_sem()`
inside `bind_con_driver()` — consistent with 3 CPUs stuck on one lock while
a 4th does TSC-timed busy-waiting elsewhere.

**What does NOT fit that theory cleanly**: the actual backlog at the stall
point is small (77 `OA_COMPORT_DBG`/marker lines, 21622 bytes of boot log
total) — implausibly small to explain many *minutes* of draining even
allowing for a large QEMU/TCG `udelay(1)` slowdown factor — and
`boot_console.log`'s byte count did not grow **at all**, even by one byte,
across the entire ~12-minute observation window that followed. A "slowly
draining" theory would predict at least some trickle of new bytes; none
appeared. So while the console_sem/serial-flush mechanism is the
best-supported *candidate* for why the lock never gets released, it is not
confirmed as *the* cause, and the alternative — some other, unidentified
global lock genuinely orphaned (taken once, its matching unlock never
reached) — remains equally plausible from the evidence gathered.

**New finding this session, not previously reported**: this is **not an
infinite hang**. After the VM sat at this exact point for very close to 12
minutes wall-clock (systemd unit `Started` 21:20:06, `Deactivated
successfully` 21:32:09, **33m10s of cumulative CPU time consumed** across
that ~12 minutes — confirming genuinely high multi-core activity throughout,
matching the "CPU time keeps climbing" observation from prior sessions), the
QEMU process **terminated on its own**. No new guest console output appeared
before it did (log content and size are identical to the last live sample).
No host-side OOM-kill or crash signal appears in the host kernel log for
that window. This is consistent with `run_vm.sh`'s `-no-reboot` flag
intercepting a guest-triggered reset (a silent panic or triple-fault the
guest was never able to print anything about — plausibly because the
emergency-print path needs the same stuck console resource) rather than
QEMU crashing or being killed externally. **No prior session is recorded as
having waited this long** — every earlier characterization ("stalls, CPU
climbing, not investigated further") stopped well short of the point where
this silent, fatal termination becomes visible.

**Root cause is NOT in `fakefb.c`** (its own code runs correctly to the
literal last line before the hang) **and is NOT in `OA.ko`** (init_module
provably completed first, and OA.ko's own retry/delay code is bounded and
fast on the no-hardware path exercised here) — both were checked and ruled
out with source-level evidence, not just inference. The most defensible
characterization achievable this session: a kernel-internal console/VT
subsystem lock interaction, triggered for the first time ever in this VM's
boot history by fakefb being the first framebuffer ever registered with a
live VT console present, that this session could not resolve to one named
lock/holder because **the running kernel's symbol table isn't available
anywhere on this host** (`vmlinux.bin` is stripped — no `.symtab`; no
`System.map` under the `RestoreDVD_SystemMNT` tree or the `linux-kronos`
build tree).

**Concrete next steps for a future session** (not attempted here — outside
this session's time budget, and each needs either a config change validated
over multiple boots or non-trivial kernel-build work):
1. Get a live guest shell *before* the stall completes: `l3:3:wait:/etc/OA.rc
   start` in `overlay/etc/inittab` blocks the `s1:3:respawn` interactive
   shell from ever starting while `loadoa` (called from `OA.rc`) is stuck —
   confirmed live (`telnet ...4444` got no response/connection-closed the
   entire time). Backgrounding the `fakefb` insmod step in `loadoa`, or
   moving the `s1` respawn to run unconditionally earlier in the boot
   sequence, would allow reading `/proc/<insmod-pid>/wchan` or
   `/proc/<pid>/stack` from the *live* guest kernel — which resolves symbols
   from the running kernel's own internal kallsyms table, sidestepping the
   stripped-`vmlinux.bin` problem entirely and giving a definitive answer.
2. Obtain (or build) a `vmlinux`/`System.map` that actually matches this
   exact `bzImage` (`RestoreDVD_SystemMNT/mnt/boot/bzImage`, built
   2024-10-10) to symbolicate `0x40106c5e` (the contended lock) and
   `0x4026ae2c` (CPU2's location) directly, instead of address-only static
   disassembly.
3. As a narrower, lower-effort experiment: try booting with
   `CONFIG_FRAMEBUFFER_CONSOLE` disabled or `fbcon=map:10` (or otherwise
   preventing VT take-over of the new fb device) to see if `register_
   framebuffer()` then returns normally — this would confirm the fbcon
   take-over chain specifically (vs. some other coincidental cause) without
   needing symbols, at the cost of losing the on-screen text console (not
   needed for this project's actual goal, which uses the serial console and
   `/dev/fb1` via `screenremote`, not a VT text console).

No code was changed this session (only a non-committed, host-local
`BZIMAGE`/`kronos.img` path workaround used to launch the test on `kronosvm`
— `run_vm.sh` itself is untouched). See
`kronosology/.claude/agent-memory/re-decompiler/rtai_virtual_driver_substitute.md`
for the cross-referenced note pointing here, and
`kronosology/reconstructed/OA/HARDWARE_REVIEW_LOG.md` for the real-hardware
verification item this raises (does real hardware's Super-I/O detection
succeeding on the first probe round avoid ever exercising this code path at
all, i.e. is this VM-only?).

---

## 1. Host environment

| Field | Value |
|---|---|
| Host OS | Linux 7.0.12-1-pve (Proxmox VE kernel, x86-64) |
| QEMU binary | `/usr/libexec/qemu-system-i386` version **7.2.22** |
| Acceleration | TCG only — KVM not available (`/dev/kvm` absent) |
| TUN/TAP | Not available (`/dev/net/tun` absent) |
| Loop devices | Blocked (`losetup` returns "Operation not permitted") |
| Working dir | QEMU launched from directory containing `kronos.img` |

### 1.1 QEMU command line (exact)

```
/usr/libexec/qemu-system-i386
  -M pc
  -cpu n270
  -m 1024M
  -smp 2
  -accel tcg
  -drive file=kronos.img,format=raw,if=ide,index=0,media=disk
  -display none
  -vga none
  -serial file:/root/.claude/jobs/840a85cf/tmp/serial6.log
  -serial tcp::4444,server,nowait
  -monitor unix:/root/.claude/jobs/840a85cf/tmp/qemu_monitor6.sock,server,nowait
  -net nic,model=rtl8139
  -net user,hostfwd=tcp::7373-:7373,hostfwd=tcp::7374-:7374
  -rtc base=utc
```

Key points:
- CPU model `n270` (Intel Atom N270) running under TCG software emulation
- 1 GiB RAM, 2 vCPUs configured (but guest disables SMP — see §3.2)
- Disk: raw IDE image `kronos.img`
- No display or VGA
- **ttyS0** → log file (boot messages, kernel console)
- **ttyS1** → TCP port 4444, `server,nowait` (interactive shell)
- QEMU monitor → Unix socket
- NIC: `rtl8139` model via SLIRP userspace networking
- Port-forwards: host 7373→guest 7373 (stream), host 7374→guest 7374 (ctrl)

### 1.2 Available QEMU NIC models

```
e1000  e1000-82544gc  e1000-82545em  e1000e  i82550  i82551
i82557a  i82557b  i82557c  i82558a  i82558b  i82559a  i82559b
i82559c  i82559er  i82562  i82801  ne2k_pci  pcnet  rtl8139
```

Note: `virtio-net-pci` is **not** available in this build.

---

## 2. Disk image (`kronos.img`)

- Format: raw
- Interface: IDE (`/dev/hda` inside guest)
- Partition layout (from kernel messages at boot):

| Device | Mount | FS | Mount options |
|---|---|---|---|
| `/dev/hda2` (= `/dev/root`) | `/` | ext2 | rw |
| `/dev/sda5` | `/korg/ro` | ext2 | ro |
| `/dev/sda6` | `/korg/rw` | ext3 | rw,noatime,commit=1,data=writeback |

> The kernel IDE driver registers the drive as `hda`; the kernel cmdline uses
> `root=/dev/hda2`. The `/proc/mounts` entries for `/korg/{ro,rw}` show `sda5`
> and `sda6` — these refer to the same physical image via the libata path.

---

## 3. Guest kernel

### 3.1 Kernel version and build

```
Linux version 2.6.32.11-korg (root@kronos) (gcc version 4.5.0 (GCC))
#31 SMP PREEMPT Thu Oct 10 08:41:36 JST 2024
```

- Architecture: i686 (32-bit x86), PAGE_OFFSET=0x40000000
- RTAI real-time patch: **I-pipe 2.6-03** (`I-pipe 2.6-03: pipeline enabled`)
- Preemption: PREEMPT (voluntary + RTAI)

### 3.2 Kernel command line (exact)

```
BOOT_IMAGE=/bzImage
root=/dev/hda2
max_loop=16
elevator=noop
loglevel=8
console=uart8250,io,0x3f8,115200n8
console=ttyS0,115200
8250.nr_uarts=4
nosmp
nmi_watchdog=0
vga=0x0103
video=vesafb
```

Key parameters:
- `nosmp` — SMP disabled; kernel boots with 1 CPU despite `-smp 2`
- `nmi_watchdog=0` — NMI watchdog off
- `console=ttyS0,115200` — primary console on ttyS0 (→ log file)
- `loglevel=8` — all kernel messages logged

### 3.3 Memory

```
Memory: 1033448k/1048448k available
  (3293k kernel code, 14248k reserved, 1227k data, 324k init, 0k highmem)
```

- Total RAM: 1024 MiB (1 GiB, from `-m 1024M`)
- Available to OS: ~1009 MiB
- No HIGHMEM

### 3.4 CPU

```
Detected 2294.055 MHz processor.
4588.11 BogoMIPS (lpj=2294055)
CPU: L1 I cache: 32K, L1 D cache: 32K
CPU: L2 cache: 4096K
CPU: L3 cache: 16384K
```

TCG emulates `n270` (Intel Atom N270). CPUID reported to guest reflects the
host CPU's capabilities, not actual N270 specs.

---

## 4. Boot process (loadoa / OA.si stub)

Boot uses a VM-specific stub (`/sbin/loadoa`) instead of the production OA.si.
The stub runs as `/etc/vm_init.sh`. Sequence on each clean boot:

```
INIT: Entering runlevel: 3

[loadoa] VM stub loadoa starting (no-RTAI mode)

[loadoa] fakefb: loading...
fakefb: init called
fakefb: fb_mem=8093f000
fakefb: registered as /dev/fb0 (800x600 8bpp)
[loadoa] fakefb: loaded OK

[loadoa] 8139cp: loading...
8139cp: 10/100 PCI Ethernet driver v1.3 (Mar 22, 2004)
eth0: RTL-8139C+ at 0x809ca000, 52:54:00:12:34:56, IRQ 10
8139cp 0000:00:02.0: setting latency timer to 64
eth0: link up, 100Mbps, full-duplex, lpa 0x05E1
[loadoa] 8139cp: insmod done
[loadoa] eth0: 10.0.2.15/24 up

GetPubIdMod: Unknown symbol stgNV2AC_sync_cmd
GetPubIdMod: Unknown symbol stgNV2AC_sync_read_cmd
[loadoa] GetPubIdMod: FAILED — /proc/id absent

[loadoa] fb1 -> fb0 symlink created

[loadoa] screenremote: launched PID=1120
/sbin/loadoa: line 100: head: command not found
[loadoa] VM stub loadoa done — exiting 0
```

Then OA.rc runs the standard Debian-style rc3.d (NIFPD, messagebus, avahi,
vsftpd — most fail with "Not a directory" because initscripts are absent).

### 4.1 Kill-switch flag

`/korg/rw/HD/_nomod` is present. This prevents screenremote from loading
optional kernel modules (`vkbd.ko`, `midi_inject.ko`). The flag does not affect
the screenremote binary itself; it just skips the module insmod calls.

### 4.2 GetPubIdMod failure

`GetPubIdMod.ko` fails to load because it imports
`stgNV2AC_sync_cmd`/`stgNV2AC_sync_read_cmd` which are only exported by
`OmapNKS4Module.ko` (the AT88 chip driver from real hardware). In the VM, these
symbols are absent and `/proc/id` is never created.

**Effect on screenremote auth:** When `KronosNet.conf` credentials match, auth
succeeds normally. If they don't match, the PublicID fallback fails (no
`/proc/id`). Tested credentials: user=`kronos` pass=`kronos` (from
`/korg/rw/Startup/KronosNet.conf`).

---

## 5. Loaded kernel modules (clean boot)

From `/proc/modules` on a clean boot:

```
8139cp   14125  0  - Live 0x809bf000
fakefb    1518  2  - Live 0x80939000
smsc7500 58207  0  - Live 0x8085a000
asix     12867  0  - Live 0x8083a000
usbnet   13225  1 asix,  Live 0x80829000
r8169    33679  0  - Live 0x80812000
mii       3768  4 8139cp,asix,usbnet,r8169,  Live 0x807ff000
```

- `8139cp` — RTL8139C+ NIC driver (PIO + C+ DMA mode). **TX broken** (see §7).
- `fakefb` — Fake framebuffer module; provides `/dev/fb0` and `/dev/fb1`
  (as a symlink) at 800×600 8bpp. Loaded from `/korg/rw/screenremote/fakefb.ko`.
- `smsc7500`, `asix`, `usbnet` — USB Ethernet drivers; preloaded from
  `/korg/rw/screenremote/` (not used in this VM — no USB Ethernet devices).
- `r8169` — Realtek Gigabit Ethernet driver; preloaded, no matching PCI device.
- `mii` — MII library, dependency of the above NIC drivers.

### 5.1 Module files in `/korg/rw/screenremote/`

```
-rwxr-xr-x  root  20568  Jun 30  8139cp.ko
-rw-r--r--  root  26612  Jun 30  8139too.ko
-rwxr-xr-x  root   4060  Jun 30  fakefb.ko
-rwxr-xr-x  root   9996  Jun 29  midi_inject.ko   (not loaded: _nomod flag)
-rwxr-xr-x  root   3848  Jun 30  vkbd.ko          (not loaded: _nomod flag)
```

### 5.2 8139too.ko — load failure

`8139too.ko` (PIO-mode RTL8139 driver, intended as TX-working alternative) is
present but **cannot be loaded** in this environment:

- `insmod /korg/rw/screenremote/8139too.ko` causes a kernel oops in
  `sysfs_add_file_mode` within `sys_init_module`
- Root cause: RTAI's I-pipe interrupt pipeline interferes with the sysfs
  registration path during module init for this driver
- Symptom: insmod process receives `Killed` signal; subsequent dmesg shows an
  oops backtrace including `__ipipe_handle_exception` and `sysfs_add_file_mode`
- The 8139cp driver does NOT exhibit this issue (it loads cleanly)

---

## 6. Network configuration (clean boot)

### 6.1 SLIRP topology

```
Host 127.0.0.1 ─── QEMU SLIRP ─── Guest 10.0.2.15/24
Gateway: 10.0.2.2   DNS: 10.0.2.3
```

### 6.2 Guest eth0 (after loadoa)

```
eth0      Link encap:Ethernet  HWaddr 52:54:00:12:34:56
          inet addr:10.0.2.15  Bcast:10.0.2.255  Mask:255.255.255.0
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          Interrupt:10  Base address:0xa000
```

IP is assigned statically by loadoa (`ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up`).

### 6.3 ARP state

```
IP address       HW type  Flags  HW address         Device
10.0.2.2         0x1      0x0    00:00:00:00:00:00  eth0
```

The gateway ARP entry (10.0.2.2) is permanently incomplete — no ARP request
can be sent because TX is broken (see §7).

---

## 7. NIC TX failure — root cause and evidence

### 7.1 Symptom

On every boot, eth0 TX remains permanently zero:

```
eth0: RX bytes=1288 (1.2 KiB)  TX bytes=0 (0.0 b)
      RX packets=28             TX packets=0
```

Any attempt to send data via eth0 produces (in dmesg):

```
8139cp: eth0: BUG! Tx Ring full when queue awake!
eth0: Transmit timeout, status  d   2b    0    0
```

### 7.2 Root cause

The `8139cp` Linux driver uses the RTL8139C+ **C+ DMA TX ring** (not the
legacy 4-descriptor PIO path used by `8139too`). QEMU 7.2.22's RTL8139
emulation under TCG does not properly clear the OWN bit in C+ TX descriptors
after "transmitting" a packet. As a result:

1. Guest writes packet descriptors to C+ TX ring (OWN=1)
2. QEMU "transmits" via SLIRP internally — but does not clear OWN
3. `rtl8139cp_tx_interrupt()` fires in guest, iterates all descriptors,
   finds all OWN=1 → declares ring full
4. TX queue is stopped permanently

### 7.3 What this means for host↔guest communication

| Direction | Path | Works? |
|---|---|---|
| Host → Guest (inbound) | SLIRP injects to NIC RX buffer | **YES** — NIC RX is unaffected |
| Guest → Host (outbound) | NIC TX → SLIRP → host socket | **NO** — C+ TX never completes |
| Guest loopback (10.0.2.15→10.0.2.15) | kernel loopback only | **YES** — bypasses NIC entirely |
| Serial port (ttyS1, port 4444) | QEMU serial device, not NIC | **YES** — independent of NIC |

**Practical consequence:** A TCP connection from host to guest port 7373
completes its handshake (SLIRP manages the SYN-ACK on behalf of the guest),
and the host can send data. However, responses from the screenremote daemon
(auth response, frames, ctrl replies) are written to the TCP socket, go to the
NIC TX ring, and are silently dropped. The host socket receives nothing.

### 7.4 Workarounds attempted (all failed)

| Attempt | Outcome |
|---|---|
| `rmmod 8139cp; insmod 8139too.ko use_io=1` | 8139too causes RTAI oops on insmod; shell process killed |
| Change QEMU NIC model to e1000/pcnet | No e1000 or pcnet32 driver in Kronos kernel |
| Enable KVM acceleration | `/dev/kvm` not available in host container |
| TUN/TAP networking | `/dev/net/tun` not available |
| Loop-mount disk image to inject NIC driver | `losetup` blocked by container security |
| QEMU monitor `device_del`/`device_add` hotplug | No alternate NIC driver to receive a new PCI device |

### 7.5 Recommended fix (requires host privileges or different environment)

- **Option A**: Launch QEMU with KVM: replace `-accel tcg` with `-accel kvm`. KVM's hardware interrupt delivery handles C+ TX correctly.
- **Option B**: Launch QEMU with a different NIC model **and** supply the matching driver module. The Kronos kernel has no e1000/pcnet/virtio drivers compiled in or available as `.ko` files.
- **Option C**: Use a QEMU build or patch that fixes the RTL8139 C+ TX descriptor OWN-bit handling in TCG mode.

---

## 8. Serial console access

### 8.1 ttyS0 — boot console (log file)

QEMU maps ttyS0 to a log file. All kernel boot messages and printk output go
here. The log is **not** interactive and is only readable from the host.

### 8.2 ttyS1 — interactive shell (port 4444)

QEMU maps ttyS1 to TCP port 4444 (`server,nowait`). The loadoa init script
spawns `/bin/sh -i` on ttyS1. This is the **only bidirectional communication
path** between host and guest (not affected by NIC TX).

**Shell environment:**
- `/bin/sh` → bash 3.2 (`GNU bash, version 3.2.57(1)-release (i486-pc-linux-gnu)`)
- `uname -a`: `Linux kronos 2.6.32.11-korg #31 SMP PREEMPT ... i686 i686 i386 GNU/Linux`
- Busybox not present; individual binaries in `/bin`, `/sbin`, `/usr/bin`
- **Available**: `bash`, `dd`, `ls`, `printf`, `echo`, `cat`, `find`, `grep`, `ps`, `ifconfig`, `route`, `netstat`, `insmod`, `rmmod`, `chmod`, `cp`, `sync`, `/dev/tcp`, `/dev/udp`
- **NOT available**: `head`, `tail`, `tee`, `awk`, `wc`, `od`, `hexdump`, `xxd`, `seq`, `nc`, `socat`, `python`, `base64`, `uudecode`, `lsmod`, `modinfo`

**Shell behaviour quirks:**
- `bash 3.2` does not support `read -N` (read exact N chars) — use `read -r -n 1`
- `dd if=/dev/fd/N` silently returns 0 bytes — use `<&N` redirect instead
- Variable substitution for `$'\r'` requires quoting care in this shell version

**Shell stability:** The shell (PID 1144 on a typical boot) dies if the kernel
oopses or if RTAI's scheduler kills the foreground process during module
operations. After a shell death, the only recovery is `system_reset` via the
QEMU monitor (port: Unix socket at `qemu_monitor6.sock`).

---

## 9. screenremote daemon

### 9.1 Binary

```
/korg/rw/screenremote/screenremote
  Size:    1,771,688 bytes
  Type:    ELF 32-bit LSB executable, Intel 80386, statically linked
  BuildID: 8852d5be014f8d31e4031c27181e174d866e32e9
  Source:  KronosScreenRemoteDaemon/source/screenremote.c v1.7.9b (2323 lines)
```

The binary in the VM is the current production build from the repository.
`build/screenremote` on the host has the same BuildID.

### 9.2 Configuration (`/korg/rw/screenremote/screenremote.cfg`)

```
stream_port=7373
ctrl_port=7374
```

No `bind_ip` option exists — the daemon always binds to the first non-loopback
LAN IPv4 address it finds via `getifaddrs()`. In the VM this is `10.0.2.15`.

### 9.3 Runtime state (clean boot)

- PID: 1120 (consistent across boots; may vary)
- State: `S (sleeping)` in `select()`
- Bound sockets:
  - fd 4 → socket [inode 128] = `10.0.2.15:7373` (TCP LISTEN, stream port)
  - fd 5 → socket [inode 129] = `10.0.2.15:7374` (TCP LISTEN, ctrl port)
  - fd 6 → socket [inode 130] = `0.0.0.0:7372` (UDP, discovery)
- fd 3 → `/dev/fb0` (open for framebuffer reads)
- stderr → `/dev/ttyS1` (log lines visible on serial shell)

### 9.4 Access log (`/korg/rw/screenremote/access.log`)

Empty on a clean boot. An entry is written for every authentication attempt
(success or failure) once a client connects on port 7373. Because host→guest
TCP produces no responses (NIC TX broken), no auth completes and the log stays
empty in normal VM operation.

### 9.5 Kill-switch behaviour

With `/korg/rw/HD/_nomod` present:
```
screenremote: kill-switch /korg/rw/HD/_nomod present — not loading any kernel modules (vkbd, midi_inject)
```
vkbd.ko and midi_inject.ko are not loaded. The `BUTTON`, `TOUCH`, `WHEEL`
commands still function via `/dev/rtf5` if it exists; if absent, the event
write silently fails but the daemon continues. The `KEY` command falls back to
uinput if `/proc/.vkbd` is absent.

### 9.6 MIDI bridge

`midi_inject.ko` is not loaded (kill-switch). The MIDI bridge subprocess
(`midi_tcp`) is not started. TCP port 9875 is not bound. `MIDI_STATUS` returns
`MIDI_LOADED=0`. `MIDI_SEND` and `SYSEX` return `ERR MIDI_NOT_LOADED`.

### 9.7 VGA mirror

`/korg/rw/screenremote/.mirror_enable` is absent on clean boot. `MIRROR_ON` /
`MIRROR_OFF` commands will fail to open `/dev/fb0_real` (only `/dev/fb0` exists
as a fakefb alias to fb1). Mirror functionality is effectively a no-op in the VM.

---

## 10. Framebuffer

| Property | Value |
|---|---|
| Device | `/dev/fb1` (required by screenremote) → symlinked from `/dev/fb0` |
| Driver | `fakefb.ko` (custom, from `/korg/rw/screenremote/fakefb.ko`) |
| Resolution | 800 × 600 pixels |
| Colour depth | 8 bpp (indexed, palette-based) |
| Stride | 800 bytes/line |
| Total frame | 480,000 bytes |
| Palette | 256-entry RGB8 table from `palette_data.h` (Kronos hardware palette) |
| fb_mem | 0x8093f000 (kernel virtual address) |

The fakefb module allocates a static 480,000-byte buffer and registers it as
both `/dev/fb0` and `/dev/fb1`. The init script creates the `/dev/fb0 → fb1`
symlink that screenremote requires.

Content of the framebuffer is initially zero (black). The Kronos UI would
normally write here; in the VM there is no UI process, so the framebuffer is
black unless manually written.

---

## 11. Protocol verification results

All findings in this section were obtained by running in-VM shell scripts that
connected to screenremote on the loopback address (10.0.2.15:7373), since
host→guest TCP produces no response data.

### 11.1 UDP discovery (port 7372)

| Field | Value |
|---|---|
| Request | `KSCR?` (5 bytes, any suffix accepted) |
| Response | `KSCR SP=7373 CP=7374 MIDI=0\n` |
| Source IP filter | None (binds INADDR_ANY) |

Verified from source: screenremote binds UDP to `INADDR_ANY:7372`.

### 11.2 TCP stream auth (port 7373)

| Field | Value |
|---|---|
| Client hello | `KSCR` + 0x02 + mode + fps + ulen + plen + user + pass |
| Auth response (success) | 777 bytes: `KSCR` + 0x00 + W_LE16 + H_LE16 + 768B palette |
| Width encoding | Little-endian 16-bit: 800 = `0x20 0x03` |
| Height encoding | Little-endian 16-bit: 600 = `0x58 0x02` |
| Auth failure | 5 bytes: `KSCR` + status (0x01=wrong pw, 0x02=no user) |
| Handshake timeout | 5 seconds |

Verified empirically from in-VM probes (`boundary_probe.sh`, `probe.sh`):
- MAGIC bytes 0–3: `4b 53 43 52` = "KSCR" ✓
- STATUS byte 4: `00` ✓
- WIDTH bytes 5–6: `20 03` = 800 LE ✓
- HEIGHT bytes 7–8: `58 02` = 600 LE ✓
- Palette bytes 9–776: 768 bytes ✓

### 11.3 TCP stream MODE_PULL (0x01)

| Field | Value |
|---|---|
| Frame request | Send `0xFF` |
| Frame response | `[len LE32][480000 bytes pixels]` |
| Frame size prefix | `00 53 07 00` = 480,000 = 0x75300 LE ✓ |
| Invalid byte | Any byte ≠ 0xFF disconnects client |

Verified: `ff_stream.bin` = 5,760,825 bytes = 777 (auth) + 12 × 480,004 (frames)

### 11.4 TCP stream MODE_CHANGE (0x02)

| Field | Value |
|---|---|
| First frame | Full frame sent immediately after auth |
| Full frame format | `[F LE32][F bytes]` where F=width×height |
| Delta frame format | `[len LE32][first_row LE16][row_count LE16][rle_bytes]` |
| Delta vs full | `len < F` → delta; `len == F` → full |
| PackBits encoding | Header 0x00–0x7F: literal run (n+1 bytes); 0x81–0xFF: repeat run (257-n times); 0x80: NOP |
| Max literal run | 128 bytes |
| Max repeat run | 128 repeats |
| FPS | Clamped to 15; 0 in client hello → uses 15 |

### 11.5 TCP ctrl (port 7374)

| Property | Value |
|---|---|
| Access control | Client IP must match stream client IP; rejected if no stream client |
| Command format | ASCII line, `\n` terminated, case-sensitive, UPPERCASE |
| One-shot | Default: send command, read response, server closes |
| Persistent | Send `CTRL_PERSIST\n` first, then commands inline |
| Commands verified | BUTTON, TOUCH, TOUCH_DOWN/MOVE/UP, WHEEL, SLIDER, VSLIDER, KEY, CHORD, REFRESH, MIRROR_ON/OFF, SS_TIMEOUT, STATE, VERSION, SYSINFO, MIDI_STATUS |

All commands return `OK\n` on success, `ERR\n` on bad args.
`SYSINFO` returns a multi-line block ending with `OK\n`.
`STATE` returns `MODE=<n>\n` (0=init, 1=Setlist…7=Disk).
`VERSION` returns `VER=<version> BUILD=<build_id>\n`.

---

## 12. Python tools (host-side)

### 12.1 `kscr_client.py`

Location: `KronosScreenRemoteDaemon/tools/kscr_client.py`

Full Python 3 client library implementing:
- `KSCRPullClient` — MODE_PULL stream client
- `KSCRChangeClient` — MODE_CHANGE stream client with delta frame reconstruction
- `KSCRCtrlClient` — CTRL_PERSIST control session (all commands)
- `KSCRMidiBridge` — TCP 9875 raw MIDI hub client
- `ctrl_oneshot()` — single one-shot ctrl command
- `discover()` — UDP discovery
- `packbits_decode()` — PackBits RLE decoder (source-verified correct)
- `save_ppm()` / `frame_to_rgb()` — frame output utilities

Protocol reference: `docs/api.md`

### 12.2 `mock_kscr_server.py`

Location: `KronosScreenRemoteDaemon/tools/mock_kscr_server.py`

Full mock KSCR server for host-side testing (not affected by NIC TX issue):
- Implements full auth + MODE_PULL + MODE_CHANGE + ctrl + UDP discovery
- Greyscale test pattern; delta frames with 5-row dirty regions
- Default listen: `127.0.0.1:7373` (stream), `127.0.0.1:7374` (ctrl)
- Credentials: `kronos` / `kronos`

Usage:
```sh
python3 mock_kscr_server.py &
python3 kscr_client.py 127.0.0.1 --mode pull --save-ppm /tmp/frame.ppm
python3 kscr_client.py 127.0.0.1 --button ENTER --version --sysinfo
```

### 12.3 End-to-end test results (against mock server)

All of the following verified passing:

| Test | Result |
|---|---|
| UDP discovery | `{'SP': 7373, 'CP': 7374, 'MIDI': 0}` ✓ |
| MODE_PULL auth (800×600, 777B response) | ✓ |
| Pull frame (480,000 bytes, LE32 header) | ✓ |
| Pull second frame (tick incremented) | ✓ |
| PPM save (1,440,000 byte RGB24 output) | ✓ |
| MODE_CHANGE first frame (full) | `(True, None)` ✓ |
| MODE_CHANGE delta frame (5 rows) | `(False, (0, 5))` ✓ |
| PackBits decode (all header ranges) | ✓ |
| CTRL one-shot VERSION | `VER=1.7.9b BUILD=...` ✓ |
| CTRL one-shot STATE / BUTTON mode change | ✓ |
| CTRL_PERSIST TOUCH/WHEEL/SLIDER/VSLIDER | ✓ |
| CTRL_PERSIST CHORD (2 buttons, hold_ms) | ✓ |
| CTRL_PERSIST SYSINFO (multi-line OK) | ✓ |
| CTRL_PERSIST MIDI_STATUS | ✓ |

---

## 13. Known divergences from real Kronos hardware

| Area | VM behaviour | Real Kronos behaviour |
|---|---|---|
| `/proc/id` | Absent (GetPubIdMod fails) | Present (AT88 PublicID) |
| PublicID auth fallback | Always fails | Works with correct PublicID |
| vkbd.ko / midi_inject.ko | Not loaded (`_nomod` flag) | Loaded normally |
| MIDI bridge port 9875 | Not bound | Bound, up to 8 clients |
| VGA mirror (fb0) | No-op (fakefb only) | Works (real VGA hardware) |
| Framebuffer content | Black (no UI process) | Live Kronos display |
| RTAI real-time domain | Present but no RT tasks | Full RTAI with RT tasks |
| NIC | TX completely broken | Full TX/RX |
| SMP | Disabled (`nosmp`) | 2 cores + HT (Kronos 1/X/2) |
| Audio | No audio hardware | USB audio, `/proc/KorgUsbAudio` |
| Touch FIFO (`/dev/rtf5`) | Absent | Present (RTAI FIFO) |
| AT88 chip | Absent | Present, accessed via OmapNKS4 |

---

## 14. Appendix: QEMU monitor access

The QEMU monitor is accessible via a Unix domain socket:

```
/root/.claude/jobs/840a85cf/tmp/qemu_monitor6.sock
```

Useful commands (note: monitor uses readline with ANSI echoing; strip escape
sequences from responses):

```
system_reset          # Hard-reset the guest (reboots)
info status           # "VM status: running" or "paused"
info network          # Show NIC and SLIRP configuration
sendkey alt-sysrq-b   # SysRq-B in guest (emergency reboot)
```

Python example:
```python
import socket, re
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/path/to/qemu_monitor6.sock')
s.settimeout(2)
s.recv(4096)  # drain banner
s.sendall(b'system_reset\n')
time.sleep(0.5)
raw = s.recv(4096)
clean = re.sub(b'\x1b\\[[^a-zA-Z]*[a-zA-Z]', b'', raw).decode()
```
