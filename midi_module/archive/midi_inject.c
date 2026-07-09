/*
 * midi_inject.ko — MIDI injection + output capture for Korg Kronos
 *
 * /proc/.midi_in    — write raw MIDI bytes to inject into OA.ko MIDI IN
 * /proc/.midi_ring  — read the Kronos MIDI OUT stream (notes, CC, bend, SysEx)
 * /proc/.midi_ports — diagnostic: out-port topology, captured port, ring_overflow_bytes
 *
 * MIDI OUT is captured by hooking CSTGMidiOutPort::ReadNextMessage (the per-port
 * output drain, already driven by the always-transmitting DIN path — no USB host
 * or can_send force needed).  ReadNextMessage runs once per active output port and
 * every port mirrors the same performance stream, so we lock onto ONE port and
 * capture only it — otherwise every event is duplicated per port.  See
 * hook_read_next_msg.
 *
 * Captured bytes flow through a LOCK-FREE single-producer/single-consumer ring
 * (uni_ring): the RT capture hook is the sole producer, /proc/.midi_ring the sole
 * consumer, and they share no lock — so the producer never drops data merely
 * because the consumer is mid-read.  This replaced a shared spin-trylock whose
 * contention drops silently corrupted large SysEx dumps (a ~79 KB Set List).  See
 * uni_ring_push_buf / ring_fops_read.
 *
 * Module params:
 *   receive_fn=0x...        MidiInPortGeneric7Receive (required for MIDI IN injection)
 *   register_fn=0x...       RegisterMidiInPort (required for port object discovery)
 *   midi_dispatch_fn=0x...  CSTGMidiOutPort::ReadNextMessage VA (all MIDI out)
 *                           Mangled: _ZN15CSTGMidiOutPort15ReadNextMessageEPhj
 *                           Kronos 1 (D510, OA@0x59d17000): 0x59E054A0
 *   capture_port_idx=-1     -1 = auto-lock to the right out-port (default).  >=0 =
 *                           force capture of that port index (see /proc/.midi_ports).
 *                           Runtime-writable for field diagnosis.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <asm/cacheflush.h>

MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */
/*  Module parameters                                                  */
/* ------------------------------------------------------------------ */

static unsigned long receive_fn = 0;
module_param(receive_fn, ulong, 0444);

static unsigned long register_fn = 0;
module_param(register_fn, ulong, 0444);

static unsigned long midi_dispatch_fn = 0;
module_param(midi_dispatch_fn, ulong, 0444);

/* ------------------------------------------------------------------ */
/*  MIDI IN injection state                                            */
/* ------------------------------------------------------------------ */

typedef void (*receive_fn_t)(void *, const uint8_t *, uint32_t)
    __attribute__((regparm(3)));

static void *port_obj;
static uint32_t *ports_array;
static struct proc_dir_entry *proc_midi_in;
static struct proc_dir_entry *proc_midi_ring;
static struct proc_dir_entry *proc_midi_ports;
/* MIDI injection uses a per-call kmalloc buffer (see midi_write), not a shared
 * static one — /proc/.midi_in is world-writable and can have concurrent writers. */

/* Guards the MIDI-IN injection target (port_obj/receive_fn).  ring_dead is set by
 * the OA-unload notifier so an in-flight /proc/.midi_in write can't call into
 * freed OA .text. */
static DEFINE_SPINLOCK(ring_lock);
static int ring_dead;

static void ring_disable(void)
{
    spin_lock(&ring_lock);
    ring_dead = 1;
    port_obj  = NULL;
    spin_unlock(&ring_lock);
}

/* ------------------------------------------------------------------ */
/*  Unified MIDI OUT ring (fed by the ReadNextMessage dispatch hook)  */
/* ------------------------------------------------------------------ */

#define UNI_RING_BITS  14               /* 16384 bytes */
#define UNI_RING_SIZE  (1 << UNI_RING_BITS)
#define UNI_RING_MASK  (UNI_RING_SIZE - 1)

static uint8_t  uni_ring[UNI_RING_SIZE];
static uint32_t uni_wpos = 0;      /* advanced ONLY by producers (under uni_prod_lock) */
static uint32_t uni_rpos = 0;      /* advanced ONLY by the consumer (ring_fops_read/write) */
static uint32_t uni_overflow = 0;  /* bytes dropped by a GENUINELY full ring (diagnostic) */

/* Producer-only lock: serializes concurrent producers against EACH OTHER (the
 * ReadNextMessage hook can fire on more than one CPU), but is NEVER taken by the
 * consumer (midi_tcp).  That property is the whole point — the RT-domain producer
 * therefore never spin-waits on a lock the *Linux* consumer holds, so there is no
 * priority-inversion / RT-starvation deadlock (on a single CPU an RT spin on a
 * Linux-held lock would starve the very task that must release it → hard freeze).
 * The old design shared ONE lock between producer and consumer and used spin_trylock
 * in the producer, which DROPPED the whole batch whenever midi_tcp held the lock
 * mid-read.  Under a dense bulk dump that punched small holes into the SysEx stream,
 * offsetting every byte after the hole (garbled Set List slots).  The
 * producer/consumer handoff is now lock-free via the wpos/rpos cursors + barriers,
 * so the consumer can never force a producer drop.  Single-producer/single-consumer
 * ring; on x86 aligned 32-bit cursor loads/stores are atomic, and ACCESS_ONCE stops
 * the compiler caching/refetching them (which would reintroduce the corruption). */
static DEFINE_SPINLOCK(uni_prod_lock);

static void uni_ring_push_buf(const uint8_t *buf, uint32_t len)
{
    unsigned long flags;
    uint32_t i, used, space, wpos, rpos;

    spin_lock_irqsave(&uni_prod_lock, flags);

    wpos = uni_wpos;                 /* this lock is the only writer of uni_wpos */
    rpos = ACCESS_ONCE(uni_rpos);    /* advanced by the consumer; a stale (smaller)
                                        read only under-reports free space — safe */
    used  = wpos - rpos;
    space = (used < UNI_RING_SIZE) ? (UNI_RING_SIZE - used) : 0;
    if (len > space) {
        uni_overflow += len - space; /* genuine overflow, NOT a lock-contention drop */
        len = space;                 /* ring full: never overwrite unread data */
    }

    for (i = 0; i < len; i++)
        uni_ring[(wpos + i) & UNI_RING_MASK] = buf[i];

    smp_wmb();                       /* ring bytes must be visible before we publish wpos */
    ACCESS_ONCE(uni_wpos) = wpos + len;

    spin_unlock_irqrestore(&uni_prod_lock, flags);
}

/* ------------------------------------------------------------------ */
/*  Inline hook infrastructure                                         */
/* ------------------------------------------------------------------ */

static uint8_t *tramp_dispatch  = NULL;  static uint8_t orig_dispatch[16];

/* The ONE CSTGMidiOutPort we capture from (see hook_read_next_msg).  ReadNextMessage
 * is called once per active output port (USB, DIN, …) and every port emits the SAME
 * performance stream, so capturing all of them duplicates every event (N ports ->
 * N copies).  Locking to one port gives a single clean stream matching what one
 * physical MIDI port transmits.  capture_port_has_note tracks whether the locked
 * port has ever carried a note, so we can UPGRADE off a first-seen control-only
 * port to a note-carrying one (see the lock logic below).  NULL while unlocked. */
static void *capture_port;
static int   capture_port_has_note;

/* DIAGNOSTIC (feeds /proc/.midi_ports): per-port message counts, immune to dmesg
 * rotation.  Cheap — a few increments per hooked call. */
#define MAXP 8
static void     *seen_ports[MAXP];
static int       nseen_ports;
static uint32_t  port_calls[MAXP];    /* # of ReadNextMessage returns with data   */
static uint32_t  port_notes[MAXP];    /* # note-on (0x9x) messages seen           */
static uint32_t  port_ccs[MAXP];      /* # control-change (0xBx) messages seen     */
static uint8_t   port_first[MAXP][3]; /* first 3 bytes this port emitted           */

/* Optional override: force capture of the port at this 0-based index (see
 * /proc/.midi_ports).  -1 = auto-lock (default). */
static int capture_port_idx = -1;
module_param(capture_port_idx, int, 0644);

/* Post-call hook for CSTGMidiOutPort::ReadNextMessage — regparm(3): self→EAX, buf→EDX, maxlen→ECX.
 * Calls original first (fills buf), then captures the returned bytes into the unified
 * ring for exactly ONE port, so multi-port output is not duplicated. */
static int __attribute__((regparm(3)))
hook_read_next_msg(void *self, uint8_t *buf, uint32_t maxlen)
{
    typedef int (*orig_t)(void *, uint8_t *, uint32_t)
        __attribute__((regparm(3)));
    int n = ((orig_t)tramp_dispatch)(self, buf, maxlen);
    if (n > 0 && (uint32_t)n <= maxlen) {
        int i, idx = -1;
        uint8_t st = buf[0] & 0xf0;

        /* Topology map (diagnostic). */
        for (i = 0; i < nseen_ports; i++)
            if (seen_ports[i] == self) { idx = i; break; }
        if (idx < 0 && nseen_ports < MAXP) {
            idx = nseen_ports++;
            seen_ports[idx] = self;
            port_first[idx][0] = buf[0];
            port_first[idx][1] = buf[1];
            port_first[idx][2] = buf[2];
        }
        if (idx >= 0) {
            port_calls[idx]++;
            if (st == 0x90) port_notes[idx]++;
            if (st == 0xb0) port_ccs[idx]++;
        }

        if (capture_port_idx >= 0) {
            /* Manual override: capture exactly the requested index. */
            if (idx == capture_port_idx)
                uni_ring_push_buf(buf, (uint32_t)n);
        } else {
            /* Auto-lock: grab the first port that emits, but UPGRADE to a
             * note-carrying port if our locked one has never carried a note.
             * This is correct whether the player uses the keyboard (locks/upgrades
             * to the performance port) or only moves controllers (locks to that
             * port; a later note upgrades if it lands elsewhere). */
            if (!capture_port) {
                capture_port = self;
                capture_port_has_note = (st == 0x90);
            } else if (st == 0x90 && self != capture_port && !capture_port_has_note) {
                capture_port = self;             /* upgrade to the note port */
                capture_port_has_note = 1;
            } else if (st == 0x90 && self == capture_port) {
                capture_port_has_note = 1;
            }
            if (self == capture_port)
                uni_ring_push_buf(buf, (uint32_t)n);
        }
    }
    return n;
}

/* Temporarily disable CR0.WP so kernel .text is writable. */
static unsigned long saved_cr0;
static void wp_disable(void)
{
    asm volatile("mov %%cr0, %0" : "=r"(saved_cr0));
    asm volatile("mov %0, %%cr0" :: "r"(saved_cr0 & ~0x10000UL));
}
static void wp_enable(void)
{
    asm volatile("mov %0, %%cr0" :: "r"(saved_cr0));
}

/* Write a 5-byte relative JMP at dst jumping to tgt.
 *
 * No reach check is needed on this platform: the Kronos is i386 (32-bit), so all
 * pointers and the E9 rel32 displacement are 32 bits, and the CPU computes the
 * near-JMP target as (dst+5)+rel32 modulo 2^32.  rel32 = tgt-(dst+5) is therefore
 * exactly representable for ANY pair of 32-bit addresses (the arithmetic wraps),
 * so an E9 near JMP reaches every address in the space regardless of distance
 * between OA .text, this module's .text, and the direct-mapped trampoline page.
 * The ±2 GB / INT32-overflow-into-garbage concern is an x86-64 consideration
 * only; if this is ever ported to a 64-bit kernel, add a range check here. */
static void write_jmp(uint8_t *dst, uint8_t *tgt)
{
    dst[0] = 0xE9;
    *(int32_t *)(dst + 1) = (int32_t)(tgt - (dst + 5));
}

/* ------------------------------------------------------------------ */
/*  Minimal 32-bit x86 instruction-length decoder                      */
/* ------------------------------------------------------------------ */
/*
 * install_hook must overwrite the first 5 bytes of the target with a JMP, then
 * relocate every ORIGINAL instruction that those 5 bytes touch into the
 * trampoline.  That requires knowing the total length of the whole instructions
 * spanning offset 0..4 — the "save length".  Hard-coding it (was 6) is correct
 * only for the one OA build the prologue was reversed on; a different OS version
 * with a different prologue could put an instruction boundary elsewhere, so a
 * fixed 6 would split an instruction and corrupt live .text.  This decoder
 * computes the length of a single instruction so save_len adapts to whatever
 * prologue the resolved function actually has.
 *
 * Scope: 32-bit protected mode, default operand/address size 32 (the Kronos is
 * i386, no long mode / no REX).  It returns LENGTH only — not operands — and
 * returns 0 for any encoding it does not model so the caller refuses to patch
 * rather than guess a wrong length.  Verified against a table of known i386
 * encodings on the host (see tools test harness).
 */

/* One-byte opcode attribute flags. */
#define _M  0x01   /* has ModR/M (+ possible SIB/displacement)        */
#define _1  0x02   /* trailing imm8                                   */
#define _Z  0x04   /* trailing imm, operand-size (4, or 2 with 0x66)  */
#define _2  0x08   /* trailing imm16 (fixed)                          */
#define _A  0x10   /* moffs: address-size immediate (4, or 2 w/ 0x67) */
#define _F  0x20   /* far pointer ptr16:32 (imm-z + 2)                */
#define _E  0x40   /* ENTER: imm16 + imm8 (3 bytes)                   */

static const uint8_t onebyte_flags[256] = {
    /*        0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F */
    /*0x*/  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,
    /*1x*/  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,
    /*2x*/  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,
    /*3x*/  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,  _M,  _M,  _M,  _M,  _1,  _Z,   0,   0,
    /*4x*/   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /*5x*/   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /*6x*/   0,   0,  _M,  _M,   0,   0,   0,   0,  _Z, (_M|_Z), _1, (_M|_1), 0, 0, 0, 0,
    /*7x*/  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,
    /*8x*/ (_M|_1),(_M|_Z),(_M|_1),(_M|_1),_M,_M,_M,_M, _M,_M,_M,_M,_M,_M,_M,_M,
    /*9x*/   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  _F,   0,   0,   0,   0,   0,
    /*Ax*/  _A,  _A,  _A,  _A,   0,   0,   0,   0,  _1,  _Z,   0,   0,   0,   0,   0,   0,
    /*Bx*/  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _Z,  _Z,  _Z,  _Z,  _Z,  _Z,  _Z,  _Z,
    /*Cx*/ (_M|_1),(_M|_1),_2,0,_M,_M,(_M|_1),(_M|_Z), _E,0,_2,0,0,_1,0,0,
    /*Dx*/  _M,  _M,  _M,  _M,  _1,  _1,   0,   0,  _M,  _M,  _M,  _M,  _M,  _M,  _M,  _M,
    /*Ex*/  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _1,  _Z,  _Z,  _F,  _1,   0,   0,   0,   0,
    /*Fx*/   0,   0,   0,   0,   0,   0,  _M,  _M,   0,   0,   0,   0,   0,   0,  _M,  _M,
};

/* Bytes consumed by a ModR/M byte at p (plus SIB and displacement).
 * addr16 selects 16-bit addressing rules.  Returns -1 on buffer overrun. */
static int modrm_len(const uint8_t *p, int max, int addr16)
{
    uint8_t modrm, sib;
    int mod, rm, len = 1;

    if (max < 1) return -1;
    modrm = p[0];
    mod = modrm >> 6;
    rm  = modrm & 7;

    if (mod == 3) return len;               /* register direct: no SIB/disp */

    if (addr16) {                           /* 16-bit addressing: no SIB */
        if (mod == 0) { if (rm == 6) len += 2; }
        else if (mod == 1) len += 1;
        else               len += 2;        /* mod == 2 */
        return len;
    }

    if (rm == 4) {                          /* SIB byte present */
        if (max < 2) return -1;
        sib = p[1];
        len += 1;
        if (mod == 0 && (sib & 7) == 5)     /* base==101, mod==00 → disp32 */
            len += 4;
    } else if (mod == 0 && rm == 5) {       /* [disp32] absolute */
        len += 4;
    }

    if (mod == 1)      len += 1;            /* disp8  */
    else if (mod == 2) len += 4;            /* disp32 */

    return len;
}

/* Length in bytes of the single instruction at code (reads at most max bytes).
 * Returns 0 if the encoding is not modelled. */
static int x86_insn_len(const uint8_t *code, int max)
{
    int i = 0, opsize16 = 0, addr16 = 0;
    uint8_t op, flags;

    /* Legacy prefixes (any order, repeatable). */
    while (i < max) {
        uint8_t b = code[i];
        if (b == 0x66) { opsize16 = 1; i++; continue; }
        if (b == 0x67) { addr16   = 1; i++; continue; }
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
        break;
    }
    if (i >= max) return 0;

    op = code[i++];

    if (op == 0x0F) {                       /* two/three-byte opcode */
        uint8_t op2;
        int ml;
        if (i >= max) return 0;
        op2 = code[i++];

        if (op2 == 0x38 || op2 == 0x3A) {   /* 0F 38 / 0F 3A three-byte */
            int imm8 = (op2 == 0x3A);
            if (i >= max) return 0;
            i++;                            /* third opcode byte */
            ml = modrm_len(code + i, max - i, addr16);
            if (ml < 0) return 0;
            i += ml + (imm8 ? 1 : 0);
            return (i <= max) ? i : 0;
        }
        if (op2 >= 0x80 && op2 <= 0x8F) {   /* Jcc near rel16/32 */
            i += opsize16 ? 2 : 4;
            return (i <= max) ? i : 0;
        }
        switch (op2) {                      /* two-byte, NO ModR/M */
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x77:
        case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            return (i <= max) ? i : 0;
        default: break;
        }
        ml = modrm_len(code + i, max - i, addr16);   /* two-byte, ModR/M */
        if (ml < 0) return 0;
        i += ml;
        switch (op2) {                      /* some two-byte carry imm8 */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0xA4: case 0xAC: case 0xBA:
        case 0xC2: case 0xC4: case 0xC5: case 0xC6:
            i += 1;
            break;
        default: break;
        }
        return (i <= max) ? i : 0;
    }

    flags = onebyte_flags[op];

    if (flags & _M) {
        int reg, ml;
        if (i >= max) return 0;
        reg = (code[i] >> 3) & 7;
        ml = modrm_len(code + i, max - i, addr16);
        if (ml < 0) return 0;
        i += ml;
        /* grp3 F6/F7: only the /0 and /1 (TEST) forms carry an immediate. */
        if      (op == 0xF6 && reg <= 1) i += 1;
        else if (op == 0xF7 && reg <= 1) i += opsize16 ? 2 : 4;
    }

    if (flags & _1) i += 1;
    if (flags & _Z) i += opsize16 ? 2 : 4;
    if (flags & _2) i += 2;
    if (flags & _A) i += addr16 ? 2 : 4;
    if (flags & _F) i += (opsize16 ? 2 : 4) + 2;
    if (flags & _E) i += 3;

    return (i <= max) ? i : 0;
}

/* Bytes to save/relocate so a 5-byte JMP patch lands on instruction boundaries:
 * sum whole instruction lengths from fn until the total covers >= 5.  Reads up
 * to `window` bytes of .text.  Returns the save length, or 0 if an instruction
 * could not be decoded. */
static int compute_save_len(const uint8_t *fn, int window)
{
    int save = 0;
    while (save < 5) {
        int l = x86_insn_len(fn + save, window - save);
        if (l <= 0) return 0;
        save += l;
    }
    return save;
}

#undef _M
#undef _1
#undef _Z
#undef _2
#undef _A
#undef _F
#undef _E

/* Install inline hook:
 *   - computes save_len = whole instructions spanning the 5-byte patch, via the
 *     x86 length decoder above (adapts to the prologue this OA build actually
 *     has — no hard-coded length)
 *   - saves save_len original bytes into orig[] (capacity orig_cap)
 *   - builds trampoline: orig[save_len] + JMP back to fn+save_len
 *   - patches fn[0..4] with JMP to hook_fn (always a 5-byte E9 rel32)
 *
 * orig_cap is the size of the caller's orig[] buffer; the hook is refused if the
 * decoded prologue needs more than that.
 *
 * expect/explen: an optional KNOWN-GOOD prologue for the target.  The hook
 * targets come from /proc/kallsyms substring matches resolved in userspace, so a
 * renamed/duplicate symbol on an untested OS build can resolve to the WRONG (but
 * valid) address, and writing a 5-byte JMP over the wrong .text corrupts a live
 * RTAI module and freezes the unit during init_module.  Because the OA prologue
 * varies by OS version and we resolve addresses dynamically, this is diagnostic,
 * not a hard gate: install_hook LOGS every target's prologue (for boot_kmsg.log),
 * hard-refuses an obviously-invalid entry (padding/ret/int3) OR a prologue it
 * cannot decode, and merely WARNS on a known-table mismatch before proceeding.
 * The length decoder now removes the "split instruction" freeze risk that a
 * fixed save_len carried; .boot self-recovery remains the backstop.
 *
 * Returns 0 on success, negative on failure (hook NOT installed). */
static int install_hook(unsigned long fn_addr, void *hook_fn,
                        uint8_t **tramp_ptr, uint8_t *orig, int orig_cap,
                        const uint8_t *expect, int explen)
{
    uint8_t *fn = (uint8_t *)fn_addr;
    uint8_t *tramp;
    unsigned long flags;
    int i, save_len;

    if (!fn_addr) return -EINVAL;

    /* Always log the prologue we are about to patch so it lands in boot_kmsg.log.
     * The OA build — and therefore the prologue bytes and where save_len lands on
     * an instruction boundary — tracks the OS version (3.1.x vs 3.2.x), not the
     * model (K1/K2/K3/Nautilus share the OS), so this is how we build a real
     * per-version prologue table from hardware. */
    printk(KERN_INFO "midi_inject: target 0x%lx prologue "
           "%02x %02x %02x %02x %02x %02x\n",
           fn_addr, fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);

    /* Hard refuse only an obviously bogus target (symbol resolved to padding /
     * a bare ret / an int3 slide) — patching that is a guaranteed brick. */
    if (fn[0] == 0x00 || fn[0] == 0xC3 || fn[0] == 0xCC) {
        printk(KERN_ERR "midi_inject: refusing hook at 0x%lx — not a function entry "
               "(first byte 0x%02x)\n", fn_addr, fn[0]);
        return -EINVAL;
    }

    /* Known-prologue check: WARN but PROCEED on mismatch.  Addresses are resolved
     * dynamically via kallsyms, so on an OS version we haven't catalogued the
     * prologue (and possibly the correct save_len) will differ — refusing here
     * would silently disable MIDI on a unit where it might work fine.  A wrong
     * save_len could still freeze, but .boot self-recovery makes that a one-boot
     * event and the prologue was just logged above for adding to the table. */
    if (expect && explen > 0) {
        for (i = 0; i < explen; i++) {
            if (fn[i] != expect[i]) {
                printk(KERN_WARNING "midi_inject: prologue at 0x%lx differs from the "
                       "known table (byte %d: 0x%02x vs 0x%02x) — proceeding; add this "
                       "prologue if this OS version works\n",
                       fn_addr, i, fn[i], expect[i]);
                break;
            }
        }
    }

    /* Decode the prologue to find how many whole-instruction bytes the 5-byte
     * JMP patch spans.  This replaces the old hard-coded length: it is correct
     * for whatever prologue the resolved OA build actually has.  If any of those
     * instructions can't be decoded, refuse — a wrong length would split an
     * instruction and corrupt live .text. */
    save_len = compute_save_len(fn, 16);
    if (save_len == 0 || save_len > orig_cap) {
        printk(KERN_ERR "midi_inject: refusing hook at 0x%lx — "
               "prologue undecodable or too long (save_len=%d, cap=%d)\n",
               fn_addr, save_len, orig_cap);
        return -EINVAL;
    }
    printk(KERN_INFO "midi_inject: prologue at 0x%lx spans %d bytes\n",
           fn_addr, save_len);

    /* Allocate trampoline from the direct-mapped region (get_free_page) so it is
     * always present in kernel page tables — no lazy vmalloc fault.  This is
     * required for hooks called from RTAI real-time tasks, whose trap handler
     * does not recover vmalloc #PF.  set_memory_x makes the page executable
     * (needed on PAE kernels with NX support).
     *
     * ┌─ BOOT-FREEZE HAZARD (suspected non-rooted brick) ───────────────────────┐
     * │ On a PAE/NX kernel set_memory_x() clears the page's NX bit, which sets   │
     * │ CPA_FLUSHTLB and forces cpa_flush_range()->on_each_cpu(...,1): a         │
     * │ SYNCHRONOUS cross-CPU TLB-flush IPI that WAITS for every core to ACK.    │
     * │ This is the SAME on_each_cpu IPI mechanism that makes freeing the        │
     * │ trampolines unsafe (now leaked, not freed) — and it runs here during     │
     * │ init_module.  On the non-rooted path the module auto-loads EARLY (right  │
     * │ when /proc/.oacmd appears) while the RT domain is spinning up and may     │
     * │ never service the IPI → on_each_cpu spins forever → whole unit freezes.  │
     * │ On the rooted path the module loads post-boot from OA.clonos.rc (steady  │
     * │ state), which is why that path doesn't reproduce it.  If the page is      │
     * │ already executable (no NX / already-X large-page direct map) this is a    │
     * │ harmless no-op (CPA_FLUSHTLB unset, no IPI).  A proper fix needs hardware │
     * │ confirmation (capture a release_debug.sh loglevel=7 boot log: freeze     │
     * │ with the "hooked 0x…" printk ABSENT confirms the flush is the culprit).  │
     * └──────────────────────────────────────────────────────────────────────────┘ */
    tramp = (uint8_t *)__get_free_page(GFP_KERNEL);
    if (!tramp) {
        printk(KERN_ERR "midi_inject: get_free_page trampoline failed\n");
        return -ENOMEM;
    }
    set_memory_x((unsigned long)tramp, 1);
    *tramp_ptr = tramp;

    /* Copy save_len bytes (covering all instructions that overlap the 5-byte patch) */
    memcpy(orig, fn, save_len);
    memcpy(tramp, orig, save_len);
    write_jmp(tramp + save_len, fn + save_len);

    /* Patch target: write 5-byte JMP to hook.  Bytes 5..save_len-1 of fn are
     * left unchanged — they are part of instructions already in the trampoline. */
    local_irq_save(flags);
    wp_disable();
    write_jmp(fn, (uint8_t *)hook_fn);
    wp_enable();
    local_irq_restore(flags);

    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");

    printk(KERN_INFO "midi_inject: hooked 0x%lx → %p tramp=%p\n",
           fn_addr, hook_fn, tramp);
    return 0;
}

/* 1 while our 5-byte JMPs are live in the target .text.  Guards against
 * restoring OA's bytes twice (once from the OA-unload notifier, once from
 * module_exit) and against writing into OA .text after OA is already gone. */
static int hooks_patched = 0;

/* Restore the 5 patched bytes in the target .text.  LOCAL-ONLY: IRQ-disabled
 * byte copy + CR0.WP toggle.  No IPI, no allocation, no sleep — therefore SAFE
 * to call from the module notifier, which runs under module_mutex during another
 * module's teardown. */
static void restore_hook_bytes(unsigned long fn_addr, uint8_t *orig)
{
    unsigned long flags;
    if (!fn_addr) return;
    local_irq_save(flags);
    wp_disable();
    memcpy((uint8_t *)fn_addr, orig, 5);  /* restore only the 5 bytes we patched */
    wp_enable();
    local_irq_restore(flags);
    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");
    printk(KERN_INFO "midi_inject: unhooked 0x%lx\n", fn_addr);
}

/* NOTE: there is intentionally NO trampoline-free routine — the pages are leaked
 * at unload rather than freed.  A trampoline is a single executable page that a
 * hook caller JMPs into and runs the relocated OA prologue from, and the
 * ReadNextMessage hook can be entered from an RTAI real-time task (ipipe RT
 * domain).  No Linux primitive can prove such an RT task has left the page:
 * synchronize_rcu()/synchronize_sched() only wait on LINUX quiescent states,
 * which RT-domain tasks never report — so freeing the page would be an
 * un-guardable use-after-free (the old code's synchronize_rcu() only *appeared*
 * to cover this).  Freeing via set_memory_nx()+free_page() would also fire the
 * very cross-CPU TLB-flush IPI that freezes this RTAI box.  Leaking ≤2 pages
 * until the next reboot is trivial on this appliance (the module loads once at
 * boot; teardown happens at an OS-update "Preparing to Install", after which the
 * unit reboots).  See midi_inject_exit. */

/* ------------------------------------------------------------------ */
/*  Module notifier — disable on OA/loadmod unload                    */
/* ------------------------------------------------------------------ */

static int midi_module_notify(struct notifier_block *nb,
                              unsigned long action, void *data)
{
    struct module *mod = data;
    if (action == MODULE_STATE_GOING &&
        (strcmp(mod->name, "OA") == 0 ||
         strcmp(mod->name, "loadmod") == 0)) {
        /* OA is being torn down.  This is exactly what the Kronos does at
         * "Preparing to Install" before an OS update.  Do the MINIMUM that is
         * safe under module_mutex on the RTAI kernel: disable the ring and
         * restore OA's patched bytes via the local-only path.  Do NOT touch the
         * trampoline pages here — set_memory_nx() would issue an IPI that can hang
         * the RT kernel and freeze the whole system (this was the cause of
         * installs/cleaners freezing at "Preparing to Install").  The trampoline
         * pages are intentionally leaked, never freed at all (see the
         * trampoline-leak note and midi_inject_exit). */
        ring_disable();
        /* Serialized against module_exit by module_mutex; hook fns never read this. */
        if (hooks_patched) {
            hooks_patched = 0;
            restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        }
        printk(KERN_INFO "midi_inject: %s unloading, hook removed (free deferred)\n",
               mod->name);
    }
    return NOTIFY_OK;
}

static struct notifier_block midi_nb = {
    .notifier_call = midi_module_notify,
};

/* ------------------------------------------------------------------ */
/*  Port discovery (MIDI IN side)                                      */
/* ------------------------------------------------------------------ */

static void *find_port_object(void)
{
    uint8_t *fn_bytes;
    int i;

    if (!register_fn) return NULL;

    fn_bytes = (uint8_t *)register_fn;
    if (fn_bytes[0] != 0x0f || fn_bytes[1] != 0xbe ||
        fn_bytes[4] != 0x89 || fn_bytes[5] != 0x04 || fn_bytes[6] != 0x95) {
        printk(KERN_ERR "midi_inject: RegisterMidiInPort pattern mismatch\n");
        return NULL;
    }

    ports_array = (uint32_t *)*(uint32_t *)(fn_bytes + 7);
    printk(KERN_INFO "midi_inject: sMidiInPorts at %p\n", ports_array);

    for (i = 0; i < 4; i++) {
        uint32_t addr = ports_array[i];
        if (addr > 0x40000000) {
            uint8_t *p = (uint8_t *)(unsigned long)addr;
            printk(KERN_INFO "midi_inject: ports[%d]=%p idx=%d flags=0x%02x %s\n",
                   i, p, p[0x25], p[0x26], (p[0x26] & 0x02) ? "ACTIVE" : "");
        }
    }

    for (i = 0; i < 4; i++) {
        uint32_t addr = ports_array[i];
        if (addr > 0x40000000) {
            uint8_t flags = ((uint8_t *)(unsigned long)addr)[0x26];
            if (flags & 0x02)
                return (void *)(unsigned long)addr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ring — MIDI out captured via the ReadNextMessage hook  */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint8_t tmp[512];
    size_t len = 0;
    uint32_t wpos, rpos, avail;
    size_t take, i;

    /* Lock-free consumer (see uni_ring_push_buf): read the producer's published
     * wpos, copy the bytes in [rpos, wpos), then advance rpos.  Sharing no lock with
     * the producer is exactly what stops the producer ever dropping a batch on our
     * account.  The dispatch hook fills uni_ring asynchronously; we hand the reader
     * whatever is buffered. */
    rpos  = uni_rpos;                /* only the consumer (this fn / ring_fops_write) moves rpos */
    wpos  = ACCESS_ONCE(uni_wpos);   /* published by a producer */
    smp_rmb();                       /* order the wpos load before reading ring data */
    avail = wpos - rpos;
    if (avail > 0) {
        take = avail;
        if (take > sizeof(tmp)) take = sizeof(tmp);
        if (take > count)       take = count;
        for (i = 0; i < take; i++)
            tmp[i] = uni_ring[(rpos + i) & UNI_RING_MASK];
        smp_mb();                    /* finish copying bytes out before we free the space */
        ACCESS_ONCE(uni_rpos) = rpos + take;
        len = take;
    }

    if (len == 0) return 0;
    if (copy_to_user(buf, tmp, len)) return -EFAULT;
    return (ssize_t)len;
}

static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    /* A write to /proc/.midi_ring means "reset my read cursor" — midi_tcp does this
     * on a new client connection so it starts from live output, not a backlog.  This
     * runs in the consumer context (single-threaded midi_tcp, never concurrent with
     * ring_fops_read), so it moves rpos lock-free: jump to the latest published
     * wpos.  A producer mid-push simply publishes a higher wpos afterward. */
    ACCESS_ONCE(uni_rpos) = ACCESS_ONCE(uni_wpos);
    return (ssize_t)count;
}

static const struct file_operations ring_fops = {
    .owner = THIS_MODULE,
    .read  = ring_fops_read,
    .write = ring_fops_write,
};

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ports — diagnostic out-port topology (read-only)       */
/* ------------------------------------------------------------------ */
/* cat /proc/.midi_ports after playing keys / moving controls to see which
 * CSTGMidiOutPort carries what.  Immune to dmesg rotation.  The '*' marks the
 * port currently being captured (auto-locked or capture_port_idx). */
static int ports_read_proc(char *page, char **start, off_t off,
                           int count, int *eof, void *data)
{
    int len = 0, i;
    len += sprintf(page + len,
                   "capture_port_idx=%d  auto_locked=%p  ports_seen=%d\n",
                   capture_port_idx, capture_port, nseen_ports);
    /* ring_overflow_bytes must read 0 in normal use.  A nonzero value means the
     * MIDI-out ring genuinely filled (producer outran the midi_tcp drain) and data
     * was dropped — distinct from the old lock-contention drop, which is gone.  If a
     * bulk dump still comes up short with this at 0, the loss is elsewhere. */
    len += sprintf(page + len, "ring_overflow_bytes=%u\n", ACCESS_ONCE(uni_overflow));
    for (i = 0; i < nseen_ports; i++) {
        int captured = (capture_port_idx >= 0) ? (i == capture_port_idx)
                                               : (seen_ports[i] == capture_port);
        len += sprintf(page + len,
                       "%c port[%d]=%p calls=%u notes=%u cc=%u first=%02x %02x %02x\n",
                       captured ? '*' : ' ', i, seen_ports[i],
                       port_calls[i], port_notes[i], port_ccs[i],
                       port_first[i][0], port_first[i][1], port_first[i][2]);
    }
    if (nseen_ports == 0)
        len += sprintf(page + len, "(no out-ports seen yet — play keys / move controls)\n");
    *eof = 1;
    return len;
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_in — MIDI injection                                    */
/* ------------------------------------------------------------------ */

#define MIDI_INJECT_MAX 4096

static int midi_write(struct file *f, const char __user *buf,
                      unsigned long count, void *data)
{
    receive_fn_t fn;
    void *obj;
    uint8_t *kbuf;
    int len = count > MIDI_INJECT_MAX ? MIDI_INJECT_MAX : count;

    spin_lock(&ring_lock);
    if (ring_dead || !port_obj || !receive_fn) {
        spin_unlock(&ring_lock);
        return -ENODEV;
    }
    fn  = (receive_fn_t)receive_fn;
    obj = port_obj;
    spin_unlock(&ring_lock);

    /* Per-call buffer, NOT a shared static one.  /proc/.midi_in is mode 0666, so
     * two processes can write concurrently; a single shared buffer would let one
     * writer's copy_from_user overwrite another's mid-message and hand OA a torn
     * multi-byte MIDI/SysEx frame.  This is process context, so a GFP_KERNEL
     * kmalloc is safe and each call gets its own isolated buffer. */
    if (len <= 0)
        return count;
    kbuf = kmalloc(len, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }

    fn(obj, kbuf, len);
    kfree(kbuf);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Deferred setup worker                                              */
/* ------------------------------------------------------------------ */
/*
 * Everything below runs in a workqueue worker, NOT in init_module context.
 * This mirrors vkbd.c and obeys the project's RTAI rule: on the Kronos kernel
 * both the GFP_KERNEL page allocation that install_hook()->__get_free_page()
 * needs AND create_proc_entry() fail when called directly from init_module,
 * and set_memory_x()'s synchronous cross-CPU TLB-flush IPI (cpa_flush_range ->
 * on_each_cpu(...,1)) can spin forever while the RT domain is still coming up on
 * the early non-rooted boot path — the documented freeze at midi_inject.c's
 * BOOT-FREEZE HAZARD box.  A workqueue worker runs in process context after RT
 * has settled, which gives it (a) full GFP_KERNEL, (b) a safe IPI (the very same
 * reasoning free_trampoline() relies on to defer set_memory_nx to module_exit),
 * and (c) removes the last init_module-context IPI on the box.  screenremote.c
 * already treats /proc/.midi_in openability — not the init_module return — as
 * the success signal, so this deferral needs no daemon-side change.
 */
static struct work_struct midi_work;

static void midi_setup(struct work_struct *work)
{
    int have_out = 0;

    /* MIDI IN injection — port discovery uses a bytecode pattern in RegisterMidiInPort
     * that is specific to the compiled OA.ko.  If it fails (different OS or model),
     * degrade gracefully: output capture still loads. */
    if (receive_fn && register_fn) {
        port_obj = find_port_object();
        if (!port_obj)
            printk(KERN_WARNING "midi_inject: port discovery failed (pattern mismatch?) "
                   "— MIDI IN injection unavailable, output capture unaffected\n");
    } else {
        printk(KERN_WARNING "midi_inject: receive_fn/register_fn not set — MIDI IN unavailable\n");
    }

    if (midi_dispatch_fn) {
        /* install_hook now decodes the prologue and computes the save length
         * itself, so any i386 prologue is handled — not just this one.
         * Known OA.ko prologue (the OS version this was reversed on):
         *   push ebp; mov ebp,esp; and esp,0xfffffff0  =  55 89 E5 83 E4 F0
         * Still passed as the known-table entry so install_hook logs the actual
         * prologue and warns (not refuses) if a different OS version differs.
         * This hook fires from an RTAI real-time task, so if boot_kmsg.log shows
         * this prologue then a freeze, suspect the set_memory_x IPI, not address
         * drift or a split instruction. */
        static const uint8_t rnm_prologue[6] = { 0x55, 0x89, 0xE5, 0x83, 0xE4, 0xF0 };
        if (install_hook(midi_dispatch_fn, hook_read_next_msg,
                         &tramp_dispatch, orig_dispatch, sizeof(orig_dispatch),
                         rnm_prologue, sizeof(rnm_prologue)) == 0)
            have_out = 1;
        else
            printk(KERN_WARNING "midi_inject: ReadNextMessage hook failed\n");
    } else {
        printk(KERN_WARNING "midi_inject: midi_dispatch_fn not set — MIDI OUT capture unavailable\n");
    }

    /* Every failure above degrades instead of aborting — this worker is void and
     * cannot fail the module load.  Deferring all setup here also makes init
     * trivially return 0 (see midi_inject_init), and screenremote.c keys off
     * /proc/.midi_in openability as the real success signal, not the init_module(2)
     * return code.
     *
     * Historical note on why never-return-negative is treated as load-bearing: a
     * negative init_module(2) return triggers module_put() on THIS .ko, and when
     * this module was built against vanilla 2.6.32 headers whose struct-module
     * layout didn't match the real Korg/RTAI ABI, mod->refptr sat at the wrong
     * offset and that module_put() faulted (oops in module_put+0x24), killing
     * screenremote before it could clear its .boot recovery flag.  The current
     * build is against /home/build/linux-kronos, which reproduces the real layout
     * (patch_init_offset.py no-ops — "Already at correct offset 0xd4" at build),
     * so that specific fault no longer applies.  The void-worker/return-0 design
     * stays regardless: it mirrors vkbd and is the right shape independent of the
     * ABI detail. */
    if (!have_out)
        printk(KERN_WARNING "midi_inject: no output path active — "
               "MIDI OUT unavailable, IN injection unaffected\n");

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (proc_midi_in)
        proc_midi_in->write_proc = midi_write;
    else
        printk(KERN_ERR "midi_inject: create_proc_entry(.midi_in) failed\n");

    if (have_out) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;

        proc_midi_ports = create_proc_entry(".midi_ports", 0444, NULL);
        if (proc_midi_ports)
            proc_midi_ports->read_proc = ports_read_proc;
    }

    if (tramp_dispatch)
        hooks_patched = 1;

    /* Register the OA/loadmod-unload notifier ONLY now, after the hook is patched
     * and hooks_patched is set.  Arming it earlier (e.g. from init while this
     * worker is still pending) would leave a window where a "Preparing to Install"
     * OA teardown finds hooks_patched==0, restores nothing, and this worker then
     * patches already-freed OA .text. */
    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_inject: ready — port=%p dispatch=%s midi_in=%s\n",
           port_obj,
           tramp_dispatch ? "ok" : "none",
           proc_midi_in   ? "ok" : "none");
}

static int __init midi_inject_init(void)
{
    /* Build tag — grep dmesg for this after a reboot to confirm the SPSC-ring
     * midi_inject is actually the one loaded (the .ko is xxd-embedded in
     * screenremote and has been silently stale before; see extract_ko). */
    printk(KERN_INFO "midi_inject: spsc-ring build\n");
    printk(KERN_INFO "midi_inject: receive=0x%lx register=0x%lx dispatch=0x%lx\n",
           receive_fn, register_fn, midi_dispatch_fn);

    /* Defer ALL setup (allocation, .text patching, set_memory_x IPI, proc entries)
     * to process context — see midi_setup().  init itself does the RTAI-safe
     * minimum and always returns 0: nothing left here can fail, so there is no
     * negative-return path at all (see midi_setup for the historical module_put
     * hazard that made always-return-0 load-bearing). */
    INIT_WORK(&midi_work, midi_setup);
    schedule_work(&midi_work);
    return 0;
}

static void __exit midi_inject_exit(void)
{
    /* Wait for the deferred setup worker to finish before tearing anything down.
     * Without this a fast insmod/rmmod could reach the teardown below while the
     * worker is still mid-install — unregistering a notifier it hasn't registered
     * yet, or racing a half-built trampoline.  flush guarantees midi_setup() ran
     * to completion (so the notifier IS registered) or never scheduled. */
    flush_scheduled_work();

    unregister_module_notifier(&midi_nb);

    /* If the OA-unload notifier already ran, OA is gone and hooks_patched is 0 —
     * do NOT write into the freed OA .text.  Otherwise (normal rmmod with OA
     * still loaded) restore the bytes and let in-flight hook calls drain. */
    if (hooks_patched) {
        hooks_patched = 0;
        restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        /* Best-effort drain of LINUX-context hook callers before this module's
         * .text is freed.  This does NOT — and cannot — drain the RT-context
         * ReadNextMessage caller: an RTAI real-time task runs in the ipipe RT
         * domain and never reports a Linux RCU quiescent state, so synchronize_rcu()
         * may return while an RT task is still executing inside the hook.  That
         * un-drainable RT caller is exactly why the trampoline page is leaked. */
        synchronize_rcu();
    }

    ring_disable();

    /* Trampoline pages are deliberately LEAKED, never freed — an RT-context caller
     * may still be inside a page and no Linux primitive can prove it has left (see
     * the trampoline-leak note above).  Freeing would be a use-after-free the RCU
     * drain above only appears to guard, and set_memory_nx()'s IPI would risk the
     * teardown freeze besides.  ≤2 pages, reclaimed at reboot. */

    if (proc_midi_ports)
        remove_proc_entry(".midi_ports", NULL);
    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_inject: unloaded\n");
}

module_init(midi_inject_init);
module_exit(midi_inject_exit);
