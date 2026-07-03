/*
 * midi_inject.ko — MIDI injection + output capture for Korg Kronos
 *
 * /proc/.midi_in   — write raw MIDI bytes to inject into OA.ko MIDI IN
 * /proc/.midi_ring — read MIDI output: unified ring fed by ReadNextMessage hook
 *                    and Block 5 SysEx drain
 *
 * Module params:
 *   receive_fn=0x...        MidiInPortGeneric7Receive (required for MIDI IN injection)
 *   register_fn=0x...       RegisterMidiInPort (required for port object discovery)
 *   midi_dispatch_fn=0x...  CSTGMidiOutPort::ReadNextMessage VA (all MIDI out)
 *                           Mangled: _ZN15CSTGMidiOutPort15ReadNextMessageEPhj
 *                           Kronos 1 (D510, OA@0x59d17000): 0x59E054A0
 *   can_send_fn=0x...       KorgUsbMidiOutputCanSend — hooked to always return 1
 *                           so the USB output thread calls ReadNextMessage even
 *                           without a USB host connected. Linux-context safe.
 *                           Kronos 1: 0x58f70390
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
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

static unsigned long can_send_fn = 0;
module_param(can_send_fn, ulong, 0444);

/* ------------------------------------------------------------------ */
/*  MIDI IN injection state                                            */
/* ------------------------------------------------------------------ */

typedef void (*receive_fn_t)(void *, const uint8_t *, uint32_t)
    __attribute__((regparm(3)));

static void *port_obj;
static uint32_t *ports_array;
static struct proc_dir_entry *proc_midi_in;
static struct proc_dir_entry *proc_midi_ring;
static uint8_t midi_buf[4096];

/* ------------------------------------------------------------------ */
/*  Block 5 ring state (SysEx responses via Korg internal path)       */
/* ------------------------------------------------------------------ */

static uint32_t *hw_queue;
static uint8_t  *hw_data;
static uint32_t  hw_mask;
static uint32_t  ring_cursor;

static DEFINE_SPINLOCK(ring_lock);
static int ring_dead;

static void ring_disable(void)
{
    spin_lock(&ring_lock);
    ring_dead = 1;
    hw_data   = NULL;
    hw_queue  = NULL;
    port_obj  = NULL;
    spin_unlock(&ring_lock);
}

/* ------------------------------------------------------------------ */
/*  Unified MIDI OUT ring (dispatcher hook + Block 5 drain)           */
/* ------------------------------------------------------------------ */

#define UNI_RING_BITS  14               /* 16384 bytes */
#define UNI_RING_SIZE  (1 << UNI_RING_BITS)
#define UNI_RING_MASK  (UNI_RING_SIZE - 1)

static uint8_t  uni_ring[UNI_RING_SIZE];
static uint32_t uni_wpos = 0;
static uint32_t uni_rpos = 0;
static DEFINE_SPINLOCK(uni_lock);

static void uni_ring_push_buf(const uint8_t *buf, uint32_t len)
{
    unsigned long flags;
    uint32_t i, used, space;

    /* Use trylock so this path never blocks.  Called potentially from the
     * kOAMidiOutput thread (Linux) or an RTAI RT context via the
     * ReadNextMessage hook.  Blocking here causes priority inversion against
     * ring_fops_read (held by midi_tcp) which stalls the RT domain and makes
     * joystick / CC inputs lag.  Drop data rather than interfere. */
    if (!spin_trylock_irqsave(&uni_lock, flags))
        return;

    used  = uni_wpos - uni_rpos;
    space = (used < UNI_RING_SIZE) ? (UNI_RING_SIZE - used) : 0;
    if (len > space)
        len = space;   /* ring full: drop new data, never overwrite unread */

    for (i = 0; i < len; i++) {
        uni_ring[uni_wpos & UNI_RING_MASK] = buf[i];
        uni_wpos++;
    }
    spin_unlock_irqrestore(&uni_lock, flags);
}

/* Drain any new Block 5 bytes into uni_ring.  Called from ring_fops_read. */
static void drain_blk5_to_uni(void)
{
    uint8_t  tmp[128];
    uint8_t  *data;
    uint32_t *queue;
    uint32_t  mask, wpos, avail, take, first;

    spin_lock(&ring_lock);
    if (ring_dead || !hw_data || !hw_queue) { spin_unlock(&ring_lock); return; }
    data  = hw_data;
    queue = hw_queue;
    mask  = hw_mask;
    spin_unlock(&ring_lock);

    if (probe_kernel_read(&wpos, &queue[3], sizeof(wpos))) {
        ring_disable();
        return;
    }
    avail = wpos - ring_cursor;
    if (!avail) return;
    if (avail > mask + 1) avail = mask + 1;

    while (avail > 0) {
        take  = avail < (uint32_t)sizeof(tmp) ? avail : (uint32_t)sizeof(tmp);
        first = (mask + 1) - (ring_cursor & mask);
        if (first > take) first = take;

        if (probe_kernel_read(tmp, &data[ring_cursor & mask], first)) {
            ring_disable(); return;
        }
        if (first < take) {
            if (probe_kernel_read(tmp + first, &data[0], take - first)) {
                ring_disable(); return;
            }
        }
        uni_ring_push_buf(tmp, take);
        ring_cursor += take;
        avail       -= take;
    }
}

/* ------------------------------------------------------------------ */
/*  Inline hook infrastructure                                         */
/* ------------------------------------------------------------------ */

static uint8_t *tramp_dispatch  = NULL;  static uint8_t orig_dispatch[16];
static uint8_t *tramp_can_send  = NULL;  static uint8_t orig_can_send[16];

/* Post-call hook for CSTGMidiOutPort::ReadNextMessage — regparm(3): self→EAX, buf→EDX, maxlen→ECX.
 * Calls original first (fills buf), then captures the returned bytes into the unified ring. */
static int __attribute__((regparm(3)))
hook_read_next_msg(void *self, uint8_t *buf, uint32_t maxlen)
{
    typedef int (*orig_t)(void *, uint8_t *, uint32_t)
        __attribute__((regparm(3)));
    int n = ((orig_t)tramp_dispatch)(self, buf, maxlen);
    if (n > 0 && (uint32_t)n <= maxlen)
        uni_ring_push_buf(buf, (uint32_t)n);
    return n;
}

/* Hook KorgUsbMidiOutputCanSend to always return 1 so the USB MIDI output
 * thread unconditionally calls ReadNextMessage even without a USB host.
 * Called from Linux context (kOAMidiOutput thread) — vmalloc-safe. */
static int hook_can_send(void) { return 1; }

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

/* Write a 5-byte relative JMP at dst jumping to tgt. */
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
     * │ This is the SAME on_each_cpu mechanism free_trampoline() documents as    │
     * │ freezing the RTAI box (set_memory_nx) — and it runs here during          │
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

/* Free a trampoline page.  set_memory_nx() issues a cross-CPU TLB-flush IPI and
 * waits for every CPU to ACK — on the RTAI kernel an RT-controlled core may never
 * service it, so on_each_cpu() spins forever and the whole system freezes.  This
 * MUST run only in process context (module_exit), NEVER from the module notifier. */
static void free_trampoline(uint8_t **tramp_ptr)
{
    if (*tramp_ptr) {
        set_memory_nx((unsigned long)*tramp_ptr, 1);
        free_page((unsigned long)*tramp_ptr);
        *tramp_ptr = NULL;
    }
}

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
         * restore OA's patched bytes via the local-only path.  Do NOT free
         * trampolines here — free_trampoline()/set_memory_nx() issues an IPI
         * that can hang the RT kernel and freeze the whole system (this was the
         * cause of installs/cleaners freezing at "Preparing to Install").  The
         * trampoline pages are reclaimed later at rmmod, in process context. */
        ring_disable();
        /* Serialized against module_exit by module_mutex; hook fns never read this. */
        if (hooks_patched) {
            hooks_patched = 0;
            restore_hook_bytes(can_send_fn,      orig_can_send);
            restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        }
        printk(KERN_INFO "midi_inject: %s unloading, hooks removed (free deferred)\n",
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
/*  Block 5 ring setup (SysEx responses)                              */
/* ------------------------------------------------------------------ */

static int setup_ring(void)
{
    uint32_t port0_q100, container_kva, blk4_data_kva;
    uint32_t *q;

    if (ports_array[0] < 0x40000000) return -ENODEV;

    port0_q100 = *(uint32_t *)((uint8_t *)(unsigned long)ports_array[0] + 0x100);
    if (port0_q100 < 0x80000000) return -ENODEV;

    container_kva = port0_q100;
    q = (uint32_t *)(unsigned long)(container_kva + 0x190);

    printk(KERN_INFO "midi_inject: container=0x%08x queue@%p blk=%u mask=0x%x wpos=%u\n",
           container_kva, q, q[0], q[2], q[3]);

    if (q[0] != 5 || q[2] != 0x7F) {
        printk(KERN_ERR "midi_inject: unexpected Block 5 queue (blk=%u mask=0x%x)\n",
               q[0], q[2]);
        return -EINVAL;
    }

    hw_queue = q;
    hw_mask  = q[2];

    blk4_data_kva = *(uint32_t *)((uint8_t *)(unsigned long)ports_array[0] + 0x104);
    hw_data = (uint8_t *)(unsigned long)(blk4_data_kva - 0x80);

    ring_cursor = q[3];

    printk(KERN_INFO "midi_inject: blk5 data=%p mask=0x%x\n", hw_data, hw_mask);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ring — unified MIDI out (dispatcher hook + Block 5)   */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint8_t tmp[512];
    size_t len = 0;
    unsigned long flags;
    uint32_t wpos, avail;
    size_t take, i;

    /* Drain Block 5 (Korg internal SysEx) into unified ring */
    drain_blk5_to_uni();

    /* Read from unified ring */
    spin_lock_irqsave(&uni_lock, flags);
    wpos  = uni_wpos;
    avail = wpos - uni_rpos;
    if (avail > 0) {
        take = avail;
        if (take > sizeof(tmp)) take = sizeof(tmp);
        if (take > count)       take = count;
        for (i = 0; i < take; i++)
            tmp[i] = uni_ring[(uni_rpos + i) & UNI_RING_MASK];
        uni_rpos += take;
        len = take;
    }
    spin_unlock_irqrestore(&uni_lock, flags);

    if (len == 0) return 0;
    if (copy_to_user(buf, tmp, len)) return -EFAULT;
    return (ssize_t)len;
}

static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    unsigned long flags;
    uint32_t wpos;

    /* Reset Block 5 cursor to current write position */
    spin_lock(&ring_lock);
    if (!ring_dead && hw_queue &&
        !probe_kernel_read(&wpos, &hw_queue[3], sizeof(wpos)))
        ring_cursor = wpos;
    spin_unlock(&ring_lock);

    /* Reset unified ring read cursor */
    spin_lock_irqsave(&uni_lock, flags);
    uni_rpos = uni_wpos;
    spin_unlock_irqrestore(&uni_lock, flags);

    return (ssize_t)count;
}

static const struct file_operations ring_fops = {
    .owner = THIS_MODULE,
    .read  = ring_fops_read,
    .write = ring_fops_write,
};

/* ------------------------------------------------------------------ */
/*  /proc/.midi_in — MIDI injection                                    */
/* ------------------------------------------------------------------ */

static int midi_write(struct file *f, const char __user *buf,
                      unsigned long count, void *data)
{
    receive_fn_t fn;
    void *obj;
    int len = count > sizeof(midi_buf) ? sizeof(midi_buf) : count;

    spin_lock(&ring_lock);
    if (ring_dead || !port_obj || !receive_fn) {
        spin_unlock(&ring_lock);
        return -ENODEV;
    }
    fn  = (receive_fn_t)receive_fn;
    obj = port_obj;
    spin_unlock(&ring_lock);

    if (copy_from_user(midi_buf, buf, len)) return -EFAULT;

    fn(obj, midi_buf, len);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                 */
/* ------------------------------------------------------------------ */

static int __init midi_inject_init(void)
{
    int ret;
    int have_ring = 0;

    printk(KERN_INFO "midi_inject: receive=0x%lx register=0x%lx dispatch=0x%lx can_send=0x%lx\n",
           receive_fn, register_fn, midi_dispatch_fn, can_send_fn);

    /* MIDI IN injection — port discovery uses a bytecode pattern in RegisterMidiInPort
     * that is specific to the compiled OA.ko.  If it fails (different OS or model),
     * degrade gracefully: output capture hooks still load. */
    if (receive_fn && register_fn) {
        port_obj = find_port_object();
        if (!port_obj) {
            printk(KERN_WARNING "midi_inject: port discovery failed (pattern mismatch?) "
                   "— MIDI IN injection unavailable, output capture unaffected\n");
        } else {
            ret = setup_ring();
            if (ret < 0)
                printk(KERN_WARNING "midi_inject: Block 5 ring setup failed (%d)\n", ret);
            else
                have_ring = 1;
        }
    } else {
        printk(KERN_WARNING "midi_inject: receive_fn/register_fn not set — MIDI IN unavailable\n");
    }

    /* CanSend hook: forces USB MIDI output thread to always call ReadNextMessage,
     * even without a USB host.  Falls back gracefully — USB submission fails silently
     * when no host is present, which is normal operation for the Kronos.
     * Symbol is in KorgUsbAudioDriver.ko; if not found on this model the hook
     * is simply skipped and ReadNextMessage only fires when USB host is connected. */
    if (can_send_fn) {
        /* No hardcoded prologue for KorgUsbMidiOutputCanSend (varies by driver
         * build); rely on the generic not-a-function-entry guard in install_hook.
         * This hook runs from Linux context, so a bad target is less catastrophic
         * than the RT-context dispatch hook below. */
        install_hook(can_send_fn, hook_can_send, &tramp_can_send,
                     orig_can_send, sizeof(orig_can_send), NULL, 0);
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
            have_ring = 1;
        else
            printk(KERN_WARNING "midi_inject: ReadNextMessage hook failed\n");
    }

    /* From here on this function must NEVER return a negative value, no matter
     * what failed above.  Confirmed against kernel/module.c: when a directly
     * init_module(2)-loaded module's init() returns < 0, sys_init_module()'s
     * error path runs unconditionally —
     *   mod->state = MODULE_STATE_GOING; synchronize_sched(); module_put(mod);
     * — and module_put(mod) is called on THIS module.  Because this .ko is
     * cross-compiled against headers that don't match the real Korg/RTAI
     * struct-module ABI (patch_init_offset.py only corrects the `init`
     * function-pointer relocation, not the rest of the struct/section layout),
     * mod->refptr does not live where module_put() expects, so the percpu
     * dereference faults on a garbage pointer — an instant oops in
     * module_put+0x24 that kills the calling process (screenremote) before it
     * can clear its .boot recovery flag, forcing a full extra reboot with every
     * kernel module disabled.  There is no safe negative return from an
     * embedded-buffer init_module() load on this kernel, so — exactly like
     * vkbd_init(), which always returns 0 and pushes all real setup into
     * schedule_work() — every failure below degrades instead of aborting.
     * screenremote.c already treats /proc/.midi_in's openability as the real
     * success signal rather than the init_module(2) return code. */
    if (!have_ring && !tramp_can_send)
        printk(KERN_WARNING "midi_inject: no output path active — "
               "MIDI OUT unavailable, IN injection unaffected\n");

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (proc_midi_in)
        proc_midi_in->write_proc = midi_write;
    else
        printk(KERN_ERR "midi_inject: create_proc_entry(.midi_in) failed\n");

    if (have_ring) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;
    }

    if (tramp_can_send || tramp_dispatch)
        hooks_patched = 1;

    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_inject: ready — port=%p blk5=%s can_send=%s dispatch=%s "
           "midi_in=%s\n",
           port_obj,
           hw_data        ? "ok" : "none",
           tramp_can_send ? "ok" : "none",
           tramp_dispatch ? "ok" : "none",
           proc_midi_in   ? "ok" : "none");
    return 0;
}

static void __exit midi_inject_exit(void)
{
    unregister_module_notifier(&midi_nb);

    /* If the OA-unload notifier already ran, OA is gone and hooks_patched is 0 —
     * do NOT write into the freed OA .text.  Otherwise (normal rmmod with OA
     * still loaded) restore the bytes and let in-flight hook calls drain. */
    if (hooks_patched) {
        hooks_patched = 0;
        restore_hook_bytes(can_send_fn,      orig_can_send);
        restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        synchronize_rcu();  /* wait for all CPUs to leave the hook paths */
    }

    ring_disable();

    /* Free trampoline pages here, in process context, where set_memory_nx()'s
     * IPI is safe (this is why the notifier deferred the free to us). */
    free_trampoline(&tramp_can_send);
    free_trampoline(&tramp_dispatch);

    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_inject: unloaded\n");
}

module_init(midi_inject_init);
module_exit(midi_inject_exit);
