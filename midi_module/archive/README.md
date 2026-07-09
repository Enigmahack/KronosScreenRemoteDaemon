# Archived: midi_inject

`midi_inject.c` (and its generated embed header `midi_inject_ko.h`) is the
predecessor of `../midi_bridge.c`. It is kept here for reference only - it is
**not built** and nothing includes it.

## Why it was replaced (v1.9.0, 2026-07)

`midi_inject` captured MIDI OUT by inline-hooking `CSTGMidiOutPort::ReadNextMessage`
- a 5-byte `E9` JMP patched over OA `.text`, with a relocated-prologue trampoline
executed from an RTAI real-time context. That approach carried two hazards:

1. The trampoline could be entered from the RTAI RT domain, so its page could
   never be safely freed (leaked until reboot), and installing/removing the patch
   ran cross-CPU TLB-flush IPIs that could freeze the box at boot or at OA teardown
   ("Preparing to Install").
2. The producer-side spinlock on the capture ring was a cross-domain
   priority-inversion risk (see the code-review finding that motivated the rewrite).

`midi_bridge` removes all of that: MIDI OUT is captured by claiming a spare reader
slot on OA's own transmit queue (`CSTGMidiQueue`) - no `.text` write, no trampoline,
no IPI, no RT-domain hook. On-hardware measurement plus a full trace of OA's
out-port construction proved OA registers exactly two out-ports (fixed at compile
time), so the queue's reader count is a stable 2 with free slots for the tap.

MIDI IN injection is unchanged between the two modules (both resolve `sMidiInPorts`
and call the real `MidiInPortGeneric7Receive`; neither patches `.text` for input).

See `project-midi-out-queue-tap-feasibility` in the session memory for the full
investigation.

## `qdiag/` - read-only out-queue diagnostic

`qdiag/midi_qdiag.c` is the read-only diagnostic used to prove the tap was safe
before `midi_bridge` was written. It resolves `sMidiOutPorts`, walks the out-port
objects, and reports each queue's capacity, write cursor, and reader count via an
on-demand `/proc/.midi_qdiag` read (no timer, no workqueue, no console output). It
patches nothing and calls no OA method - every dereference goes through
`probe_kernel_read`. `delmod.c` is a tiny `delete_module(2)` helper.

Archived, not built by the daemon. To run it standalone:

    cd midi_module/archive/qdiag && make
    scp -O midi_qdiag.ko root@<kronos>:/korg/rw/
    insmod /korg/rw/midi_qdiag.ko regoutport=<RegisterMidiOutPort VA>
    cat /proc/.midi_qdiag        # one sample per read
    rmmod midi_qdiag

