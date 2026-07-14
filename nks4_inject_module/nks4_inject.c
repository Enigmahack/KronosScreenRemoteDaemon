/*
 * nks4_inject.c - Direct front-panel event injection for Korg Kronos
 *
 * Calls OA.ko's real CSTGFrontPanel::Handle* methods directly - the same
 * functions CSTGOmapNKSMsgHandler::ProcessNextNKSEvent() calls when a real
 * NKS4 hardware event (button/touch/rotary/analog) arrives. No hook, no
 * trampoline, no interception of the real event path: this module makes an
 * ADDITIVE call into OA.ko from a proc-triggered context, so injected events
 * get bit-for-bit the same dispatch (CPowerOffTimer reset, MIDI-port-manager
 * gate, ButtonPressHandler's full per-button action table, etc.) a physical
 * press produces - including any RT-domain side effect (sequencer transport,
 * KARMA control-surface state, conditional MIDI emission) that never reaches
 * Eva's /dev/rtf5 shadow packet.
 *
 * Calling convention, ground-truthed via objdump -dr against the real OA.ko
 * (not decompiler output) - see project research notes for the full trace:
 *
 *   CSTGFrontPanel::HandleSwitchEvent(eSTGButtonCode, bool)      regparm(3)
 *     EAX = this = *CSTGFrontPanel::sInstance   EDX = code (0-127)   CL = pressed
 *
 *   CSTGFrontPanel::HandleTouchPanel(eNKS4TouchPanelEventType, int)
 *     EAX = this                                 EDX = event_type    ECX = v_adc|(h_adc<<8)
 *
 *   CSTGFrontPanel::HandleRotary(int)
 *     EAX = this                                 EDX = delta (e.g. 0x0100 CW / 0xFF00 CCW)
 *
 *   CSTGFrontPanel::HandleAnalogController(eSTGAnalogDeviceCode, uchar, ushort)
 *     EAX = this   EDX = device_code   ECX = param2(uchar)   [stack] = param3(ushort)
 *     param2/param3 are NOT raw ADC bytes - they come from ShortInvertNkS4AnalogValue
 *     (also a real OA.ko function, called first, same technique - no reimplementation
 *     of its bit-twiddling, just call the genuine article):
 *
 *   ShortInvertNkS4AnalogValue(uchar byte0, uchar byte1, ushort *out_hi, ushort *out_lo)
 *     EAX = byte0   EDX = byte1   ECX = &out_hi   [stack] = &out_lo
 *
 * eSTGAnalogDeviceCode reference (HandleAnalogController's device_code arg).
 * Ground-truthed from AnalogControllerHandler's own dispatch tables in OA.ko
 * (three separate jump tables keyed off device_code ranges, each entry a real
 * relocation to a named CSTGControllerInfo::Analog*Handler method) and cross-
 * checked against the official Kronos Operation Guide's front-panel control
 * list (Joystick=item 12, Vector Joystick=item 9 - a SEPARATE physical
 * control from the plain Joystick, Ribbon=item 13, DAMPER/foot switch jacks
 * documented in the rear-panel section) to confirm every one of these is a
 * real, standard Kronos control, not dead/shared-platform code with no
 * physical hardware behind it on this unit:
 *
 *   1       AnalogJoystickXHandler    Joystick (item 12), left/right - pitch bend
 *   2       AnalogJoystickYHandler    Joystick (item 12), fwd/back - vibrato/wah
 *   3       AnalogRibbonXHandler      Ribbon controller (item 13), finger position
 *   4       AnalogRibbonZHandler      Ribbon controller (item 13), second axis -
 *                                     structurally confirmed (real dispatch entry),
 *                                     but which physical property it reads (touch
 *                                     pressure is the common meaning of a ribbon's
 *                                     "Z" axis) is NOT hardware-verified
 *   5       AnalogVectorXHandler      Vector Joystick (item 9) - Vector Synthesis,
 *                                     a genuinely separate physical joystick from 1/2
 *   6       AnalogVectorYHandler      Vector Joystick (item 9), other axis
 *   7       AnalogAftertouchHandler   Keybed channel aftertouch
 *   8-15    (knobHandlers, per-page)  RT Knobs 1-8 - HARDWARE-CONFIRMED (see below)
 *   16-23   (sliderHandlers, per-page) Sliders 1-8 - HARDWARE-CONFIRMED (see below)
 *   24      (context handler)         Effects-rack param edit - NOT a fixed physical
 *                                     control, only reachable in an effects-editing
 *                                     UI context; do not treat as a stable device
 *   25      AnalogValueSliderHandler  Value Slider - HARDWARE-CONFIRMED (see below)
 *   26      AnalogTempoHandler        Tempo - HARDWARE-CONFIRMED. Same behaviour class
 *                                     as DAMPER (29): a large single-step jump produces
 *                                     inconsistent, non-repeatable results, but a smooth
 *                                     monotonic ramp is a clean, precisely reproducible
 *                                     curve, confirmed identical in both directions:
 *                                     value 0=40bpm, 16=51, 32=68, 48=92, 64=120, 80=154,
 *                                     96=196, 112=245, 127=297 (approx. the documented
 *                                     40-300bpm range; non-linear, more resolution at low
 *                                     tempos). See TEMPO's own comment in screenremote.c
 *                                     for the ramping implementation this requires.
 *   27      AnalogFootPedalHandler    Rear-panel assignable PEDAL jack - HARDWARE-
 *                                     CONFIRMED, plain 0-127 value, large single-step
 *                                     jumps (e.g. 0 -> 64 -> 127) work exactly like
 *                                     the knob/slider group
 *   28      AnalogFootSwitchHandler   Rear-panel assignable foot SWITCH jack -
 *                                     HARDWARE-CONFIRMED, simple on/off, large jumps
 *                                     fine (it's a switch, not a continuous control)
 *   29      AnalogDamperHandler       Rear-panel DAMPER jack (sustain / half-damper)
 *                                     - HARDWARE-CONFIRMED, but with a real behavioural
 *                                     quirk unique to this handler: the jack accepts
 *                                     either a simple on/off footswitch (e.g. Korg PS-1)
 *                                     or an actual continuous half-damper pedal (Korg
 *                                     DS-1H), and the handler evidently uses rate-of-
 *                                     change to tell them apart. Large single-step
 *                                     jumps (0 -> 64 -> 127 -> 64 -> 0, identical in
 *                                     shape to what worked fine for device code 27)
 *                                     produced inconsistent, non-repeatable results on
 *                                     hardware across two otherwise-identical runs. A
 *                                     slow ramp (256 steps, 1 unit each, ~40ms/step,
 *                                     full 0-127-0 sweep) was smooth and repeatable
 *                                     both directions. Send DAMPER as a gradual ramp,
 *                                     never a large instantaneous jump.
 *   30      SetControllerAssignment   NOT a physical control - a distinct system
 *                                     action (assigns what a controller maps to,
 *                                     not a value reading). Out of scope for the
 *                                     table above; noted here only so the 0-30
 *                                     range check below isn't a mystery number.
 *
 * "HARDWARE-CONFIRMED" above means an actual injected ANALOG command was sent
 * to a real Kronos and produced the expected on-screen value change (see
 * docs/api.md Section 9 for the button table's equivalent hardware capture, and the
 * SLIDER/KNOB/VSLIDER command docs for the confirmed byte0=value*2 formula).
 * Device codes 1-7 and 27-29 are NOT hardware-tested - they are identified
 * with high confidence from the real dispatch table structure and cross-
 * checked against the manual, but no screenremote.c command exercises them
 * yet, so their exact byte0/byte1 -> displayed-value scaling is unconfirmed
 * (may not follow the same byte0=value*2 formula the tested 8/16/25 group
 * uses - that formula was derived from ShortInvertNkS4AnalogValue's transform
 * plus how the SPECIFIC tested handlers interpret its output; an untested
 * handler could interpret out_hi/out_lo differently, the same way Tempo does).
 *
 * All five symbols are OA.ko-internal (not EXPORT_SYMBOL'd), so - same
 * convention already established in this project by midi_bridge.ko and
 * kronos_extract.ko - their live runtime addresses are resolved by the
 * OPERATOR at insmod time via /proc/kallsyms and passed as module params.
 * This avoids depending on kallsyms_lookup_name (not confirmed exported on
 * this kernel) and matches how every other cross-module OA.ko call in this
 * project is already wired up.
 *
 *   grep _ZN14CSTGFrontPanel17HandleSwitchEventE14eSTGButtonCodeb   /proc/kallsyms
 *   grep _ZN14CSTGFrontPanel16HandleTouchPanelE24eNKS4TouchPanelEventTypei /proc/kallsyms
 *   grep _ZN14CSTGFrontPanel12HandleRotaryEi                       /proc/kallsyms
 *   grep _ZN14CSTGFrontPanel22HandleAnalogControllerE20eSTGAnalogDeviceCodeht /proc/kallsyms
 *   grep ShortInvertNkS4AnalogValue                                /proc/kallsyms
 *
 * CSTGFrontPanel::sInstance is NOT resolvable this way - it's a .bss data
 * symbol and this kernel's /proc/kallsyms only exposes function symbols
 * (confirmed live, 2026-07: absent even though `nm OA.ko` shows it as a
 * normal global). Same technique already used elsewhere in this project
 * (midi_bridge.c's "resolve sMidiInPorts via the byte pattern in
 * RegisterMidiInPort") applies here: HandleSwitchEvent's own first few
 * instructions load sInstance via `mov 0x0,%eax` with an R_386_32
 * relocation - by link/load time the module loader has already filled in
 * the real resolved address as a literal 4-byte immediate embedded in
 * HandleSwitchEvent's own already-loaded, already-relocated machine code.
 * We're a kernel module too, so we just read those 4 bytes directly out of
 * OA's live .text at a fixed, ground-truthed offset - no separate address
 * to resolve or pass in. See SINSTANCE_REL_OFFSET below.
 *
 * Command interface: write one line to /proc/.nks4inject
 *   BTN <code>              press + release a raw scan code (0-127)
 *   BTN_DOWN <code>         press only (hold for a chord)
 *   BTN_UP <code>           release only
 *   TOUCH <type> <coord>    type: 1=down 2=up 3=drag   coord: v_adc|(h_adc<<8)
 *   ROT <delta>             rotary encoder delta, e.g. 256 (CW) / -256 or 65280 (CCW)
 *   ANALOG <dev> <b0> <b1>  raw analog channel bytes - see eSTGAnalogDeviceCode
 *                           reference above for the dev 0-29 table (RT knobs,
 *                           sliders, value slider, joystick/vector/ribbon/
 *                           aftertouch, pedal/switch/damper). Data wheel is
 *                           NOT an ANALOG device - it's a separate event class,
 *                           use ROT instead.
 *
 * Diagnostics: read /proc/.nks4inject_status for resolved addresses + call counters.
 *
 * RTAI note: GFP_KERNEL and create_proc_entry both fail when called directly
 * from init_module on this kernel - all setup runs via schedule_work(), same
 * as vkbd.c/midi_bridge.c.
 *
 * Safety: an OA-unload notifier (same pattern as midi_bridge.c) blocks all
 * injection the instant OA starts tearing down, so we never call into freed
 * module memory. Do not rmmod OA while this module is loaded regardless -
 * see project_oa_hot_swap_bug notes - the notifier only protects THIS
 * module's own calls, not the documented /proc/.shm refcount issue.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Direct front-panel event injection for Korg Kronos (calls OA.ko's real Handle* methods)");

/* ------------------------------------------------------------------ */
/*  Module parameters - resolved via /proc/kallsyms, see header comment */
/* ------------------------------------------------------------------ */

static unsigned long fn_switch = 0;        /* CSTGFrontPanel::HandleSwitchEvent */
module_param(fn_switch, ulong, 0444);

static unsigned long fn_touch = 0;         /* CSTGFrontPanel::HandleTouchPanel */
module_param(fn_touch, ulong, 0444);

static unsigned long fn_rotary = 0;        /* CSTGFrontPanel::HandleRotary */
module_param(fn_rotary, ulong, 0444);

static unsigned long fn_analog = 0;        /* CSTGFrontPanel::HandleAnalogController */
module_param(fn_analog, ulong, 0444);

static unsigned long fn_invert = 0;        /* ShortInvertNkS4AnalogValue */
module_param(fn_invert, ulong, 0444);

static unsigned long fn_chord = 0;         /* RT_chord_trigger(uchar,uchar,uchar,uchar) -
                                             * NOT kallsyms-visible (no separate symbol),
                                             * resolved via the fixed file-offset delta from
                                             * Do_KM_note_out_chord_trig (which IS visible) -
                                             * see project notes. EXPERIMENTAL: reads directly
                                             * from KARMA's chord-memory tables and calls
                                             * Do_KM_note_out_chord_trig() per voice - this is
                                             * the real per-pad "Pads (touch to play)" trigger,
                                             * confirmed via CESProgTask::GetPad1-8 calling
                                             * ESCommonKarmaCommon_GetChordMemory with the same
                                             * slot-index convention. */
module_param(fn_chord, ulong, 0444);

/* ------------------------------------------------------------------ */
/*  Function pointer types - regparm(3) reproduces the confirmed ABI   */
/*  exactly: GCC assigns the first 3 int/pointer args to EAX,EDX,ECX   */
/*  in declaration order, spilling any 4th to the stack.                */
/* ------------------------------------------------------------------ */

typedef void (*handle_switch_event_t)(void *this_, int code, int pressed)
    __attribute__((regparm(3)));
typedef void (*handle_touch_panel_t)(void *this_, int event_type, int coord)
    __attribute__((regparm(3)));
typedef void (*handle_rotary_t)(void *this_, int delta)
    __attribute__((regparm(3)));
typedef void (*handle_analog_controller_t)(void *this_, int device_code,
                                            unsigned char param2, unsigned short param3)
    __attribute__((regparm(3)));
typedef void (*short_invert_t)(unsigned char byte0, unsigned char byte1,
                                unsigned short *out_hi, unsigned short *out_lo)
    __attribute__((regparm(3)));
typedef void (*rt_chord_trigger_t)(unsigned char pad_index, unsigned char velocity,
                                    unsigned char param3, unsigned char param4)
    __attribute__((regparm(3)));

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static DEFINE_SPINLOCK(inj_lock);
static int oa_dead;

static struct proc_dir_entry *proc_inject;
static struct proc_dir_entry *proc_status;
static struct work_struct     setup_work;

static uint32_t cnt_btn, cnt_touch, cnt_rot, cnt_analog, cnt_gated, cnt_badcmd;
static uint32_t cnt_chord;
static int last_chord_pad = -1, last_chord_vel = -1, last_chord_p3 = -1, last_chord_p4 = -1;
static int last_chord_rc = -2;   /* -2 = never called since load */
static char last_cmd[64];

/* Byte offset, from HandleSwitchEvent's own entry point, of the 4-byte
 * immediate operand of `mov 0x0,%eax` that R_386_32-relocates against
 * CSTGFrontPanel::sInstance. Ground-truthed via objdump -dr against the
 * real OA.ko (offline file offset 0xc114 opcode / 0xc115 operand, function
 * entry 0xc0f0 -> 0x25). Re-verify with objdump if OA.ko is ever rebuilt. */
#define SINSTANCE_REL_OFFSET 0x25

static unsigned long sinstance_addr;   /* &CSTGFrontPanel::sInstance, derived at setup */

static int kptr_ok(unsigned long p)
{
    return p >= 0x40000000UL && p < 0xfffff000UL && (p & 3) == 0;
}

/* Dereference CSTGFrontPanel::sInstance fresh on every call - cheap, and
 * avoids caching a stale object pointer across an OA reload. */
static void *frontpanel_this(void)
{
    void *p;
    if (!sinstance_addr)
        return NULL;
    p = *(void **)sinstance_addr;
    return kptr_ok((unsigned long)p) ? p : NULL;
}

/* ------------------------------------------------------------------ */
/*  Injection primitives                                               */
/* ------------------------------------------------------------------ */

static int inject_button(int code, int pressed)
{
    void *this_;
    handle_switch_event_t f = (handle_switch_event_t)fn_switch;

    if (oa_dead || !fn_switch || code < 0 || code > 127)
        return -1;
    this_ = frontpanel_this();
    if (!this_)
        return -1;
    f(this_, code, pressed);
    return 0;
}

static int inject_touch(int event_type, int coord)
{
    void *this_;
    handle_touch_panel_t f = (handle_touch_panel_t)fn_touch;

    if (oa_dead || !fn_touch || event_type < 1 || event_type > 3)
        return -1;
    this_ = frontpanel_this();
    if (!this_)
        return -1;
    f(this_, event_type, coord & 0xffff);
    return 0;
}

static int inject_rotary(int delta)
{
    void *this_;
    handle_rotary_t f = (handle_rotary_t)fn_rotary;

    if (oa_dead || !fn_rotary)
        return -1;
    this_ = frontpanel_this();
    if (!this_)
        return -1;
    f(this_, delta);
    return 0;
}

static int inject_analog(int dev, unsigned char b0, unsigned char b1)
{
    void *this_;
    handle_analog_controller_t hac = (handle_analog_controller_t)fn_analog;
    short_invert_t              inv = (short_invert_t)fn_invert;
    unsigned short out_hi = 0, out_lo = 0;

    /* 0-30 is the full confirmed eSTGAnalogDeviceCode dispatch range (see the
     * header comment's table; 30 is CSTGControllerRTData::SetControllerAssignment,
     * a distinct system action, not a physical control - excluded from the
     * table but still a real, safe dispatch target). Verified by disassembling
     * the actual out-of-range fallthrough target in AnalogControllerHandler:
     * anything above 30 lands on the same no-op default every other
     * structurally-confirmed out-of-range value does. */
    if (oa_dead || !fn_analog || !fn_invert || dev < 0 || dev > 30)
        return -1;
    this_ = frontpanel_this();
    if (!this_)
        return -1;
    inv(b0, b1, &out_hi, &out_lo);
    hac(this_, dev, (unsigned char)out_hi, out_lo);
    return 0;
}

/* EXPERIMENTAL: RT_chord_trigger is a plain (non-member) function - no "this"
 * pointer, no frontpanel_this() gate. pad_index 0-7 selects the KARMA chord-
 * memory slot (confirmed via CESProgTask::GetPad1-8 -> ESCommonKarmaCommon_
 * GetChordMemory using the same 0-based slot convention). velocity!=0 plays
 * the chord (per-voice Do_KM_note_out_chord_trig calls); velocity==0 releases
 * it. param3/param4 mirror the real caller's own (bank-flag, this!=0) args -
 * best-effort guess (0, 1), not yet hardware-confirmed to matter. */
static int inject_chord(int pad_index, int velocity, int param3, int param4)
{
    rt_chord_trigger_t f = (rt_chord_trigger_t)fn_chord;

    if (oa_dead || !fn_chord || pad_index < 0 || pad_index > 7)
        return -1;
    f((unsigned char)pad_index, (unsigned char)velocity,
      (unsigned char)param3, (unsigned char)param4);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/.nks4inject write handler                                    */
/* ------------------------------------------------------------------ */

static int inject_write_proc(struct file *file, const char __user *buf,
                              unsigned long count, void *data)
{
    char kbuf[64];
    size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
    int a = 0, b = 0, c = 0, d = 0;
    unsigned long flags;
    int rc = -EINVAL;

    if (copy_from_user(kbuf, buf, n))
        return -EFAULT;
    kbuf[n] = '\0';

    if (oa_dead) {
        cnt_gated++;
        return -ENODEV;
    }

    spin_lock_irqsave(&inj_lock, flags);
    strlcpy(last_cmd, kbuf, sizeof(last_cmd));

    if (sscanf(kbuf, "BTN_DOWN %d", &a) == 1) {
        rc = inject_button(a, 1); cnt_btn++;
    } else if (sscanf(kbuf, "BTN_UP %d", &a) == 1) {
        rc = inject_button(a, 0); cnt_btn++;
    } else if (sscanf(kbuf, "BTN %d", &a) == 1) {
        rc = inject_button(a, 1);
        if (rc == 0) rc = inject_button(a, 0);
        cnt_btn++;
    } else if (sscanf(kbuf, "TOUCH %d %d", &a, &b) == 2) {
        rc = inject_touch(a, b); cnt_touch++;
    } else if (sscanf(kbuf, "ROT %d", &a) == 1) {
        rc = inject_rotary(a); cnt_rot++;
    } else if (sscanf(kbuf, "ANALOG %d %d %d", &a, &b, &c) == 3) {
        rc = inject_analog(a, (unsigned char)b, (unsigned char)c); cnt_analog++;
    } else if (sscanf(kbuf, "PADMODE %d", &a) == 1) {
        /* Diagnostic/experimental: directly force CSTGFrontPanel::sInstance[0x104],
         * the same flag CSTGControlMsgHandler::EnableOnScreenTouchPads() sets from
         * Eva's side (CFormOmnimodePads::OnShow(), confirmed via Eva decompilation)
         * and the same flag HandleTouchPanel's own disassembly branches on. Not a
         * substitute for the real Eva-driven arm sequence long-term (see project
         * notes) - this exists purely to test whether forcing the flag alone,
         * independent of Eva, is sufficient for TOUCH to reach the KARMA CC path. */
        void *this_ = frontpanel_this();
        if (this_) {
            *(unsigned char *)((char *)this_ + 0x104) = (a != 0);
            rc = 0;
        } else {
            rc = -1;
        }
    } else if (sscanf(kbuf, "PADCHORD %d %d %d %d", &a, &b, &c, &d) == 4) {
        rc = inject_chord(a, b, c, d);
        cnt_chord++;
        last_chord_pad = a; last_chord_vel = b; last_chord_p3 = c; last_chord_p4 = d;
        last_chord_rc = rc;
    } else {
        cnt_badcmd++;
    }
    spin_unlock_irqrestore(&inj_lock, flags);

    return rc == 0 ? (int)count : -EINVAL;
}

/* Diagnostic-only: CSTGFrontPanel's onscreen-touch-pad-mode fields, ground-
 * truthed against HandleTouchPanel's own disassembly (offsets 0x104/0x105/
 * 0x106 read/written right there) and CSTGControlMsgHandler::
 * EnableOnScreenTouchPads(), which does nothing but
 * `sInstance[0x104] = (param2 != 0)`. When this byte is 0, HandleTouchPanel
 * takes its default branch (PushUnsolicitedMessage -> forwarded to Eva as a
 * generic UI touch - this is the path every BUTTON/knob/slider/tab test in
 * this project has exercised). When nonzero, it takes a completely different
 * branch that never reaches Eva at all - it locally computes a value from
 * the touch position and calls CSTGControllerRTData::SendKarmaCCToKGE()
 * directly. Reading this byte tells us, without guessing, which branch a
 * given injected TOUCH would actually take. */
static int touch_pad_mode_read(unsigned char *flag_out, unsigned char *latch_out,
                                unsigned char *stored_out)
{
    void *this_ = frontpanel_this();
    if (!this_)
        return -1;
    *flag_out   = *(unsigned char *)((char *)this_ + 0x104);
    *latch_out  = *(unsigned char *)((char *)this_ + 0x105);
    *stored_out = *(unsigned char *)((char *)this_ + 0x106);
    return 0;
}

static int status_read_proc(char *page, char **start, off_t off,
                             int count, int *eof, void *data)
{
    unsigned char pad_flag = 0xff, pad_latch = 0xff, pad_stored = 0xff;
    int pad_rc = touch_pad_mode_read(&pad_flag, &pad_latch, &pad_stored);
    int len = snprintf(page, count,
        "sinstance=0x%lx fn_switch=0x%lx fn_touch=0x%lx fn_rotary=0x%lx "
        "fn_analog=0x%lx fn_invert=0x%lx fn_chord=0x%lx\n"
        "oa_dead=%d this_resolved=%d\n"
        "onscreen_touch_pad_mode=%d latch=0x%02x stored=0x%02x (rc=%d)\n"
        "counters: btn=%u touch=%u rot=%u analog=%u gated=%u badcmd=%u chord=%u\n"
        "last_chord: pad=%d vel=%d p3=%d p4=%d rc=%d\n"
        "last_cmd=%s\n",
        sinstance_addr, fn_switch, fn_touch, fn_rotary, fn_analog, fn_invert, fn_chord,
        oa_dead, frontpanel_this() != NULL,
        (int)pad_flag, pad_latch, pad_stored, pad_rc,
        cnt_btn, cnt_touch, cnt_rot, cnt_analog, cnt_gated, cnt_badcmd, cnt_chord,
        last_chord_pad, last_chord_vel, last_chord_p3, last_chord_p4, last_chord_rc,
        last_cmd);
    *eof = 1;
    return len;
}

/* ------------------------------------------------------------------ */
/*  OA-unload notifier - same pattern as midi_bridge.c                 */
/* ------------------------------------------------------------------ */

static int nks4_inject_notify(struct notifier_block *nb,
                               unsigned long action, void *data)
{
    struct module *mod = data;
    if (action == MODULE_STATE_GOING &&
        (strcmp(mod->name, "OA") == 0 || strcmp(mod->name, "loadmod") == 0)) {
        unsigned long flags;
        spin_lock_irqsave(&inj_lock, flags);
        oa_dead = 1;
        spin_unlock_irqrestore(&inj_lock, flags);
        printk(KERN_INFO "nks4_inject: %s unloading, injection disabled\n", mod->name);
    }
    return NOTIFY_OK;
}

static struct notifier_block nks4_nb = {
    .notifier_call = nks4_inject_notify,
};

/* ------------------------------------------------------------------ */
/*  Deferred setup (RTAI: create_proc_entry off init_module)           */
/* ------------------------------------------------------------------ */

static void nks4_inject_setup(struct work_struct *work)
{
    if (fn_switch) {
        unsigned long candidate = *(unsigned long *)(fn_switch + SINSTANCE_REL_OFFSET);
        if (kptr_ok(candidate)) {
            sinstance_addr = candidate;
        } else {
            printk(KERN_ERR "nks4_inject: derived sInstance addr 0x%lx looks bogus - "
                   "SINSTANCE_REL_OFFSET may be stale for this OA.ko build, "
                   "injection disabled\n", candidate);
        }
    } else {
        printk(KERN_ERR "nks4_inject: fn_switch not set - cannot derive sInstance, "
               "injection disabled\n");
    }

    proc_inject = create_proc_entry(".nks4inject", 0200, NULL);
    if (proc_inject)
        proc_inject->write_proc = inject_write_proc;
    else
        printk(KERN_ERR "nks4_inject: create_proc_entry(.nks4inject) failed\n");

    proc_status = create_proc_entry(".nks4inject_status", 0444, NULL);
    if (proc_status)
        proc_status->read_proc = status_read_proc;

    register_module_notifier(&nks4_nb);

    printk(KERN_INFO "nks4_inject: ready - sinstance=0x%lx switch=0x%lx touch=0x%lx "
           "rotary=0x%lx analog=0x%lx invert=0x%lx\n",
           sinstance_addr, fn_switch, fn_touch, fn_rotary, fn_analog, fn_invert);
}

static int __init nks4_inject_init(void)
{
    INIT_WORK(&setup_work, nks4_inject_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit nks4_inject_exit(void)
{
    flush_scheduled_work();
    unregister_module_notifier(&nks4_nb);
    if (proc_status)
        remove_proc_entry(".nks4inject_status", NULL);
    if (proc_inject)
        remove_proc_entry(".nks4inject", NULL);
    printk(KERN_INFO "nks4_inject: unloaded\n");
}

module_init(nks4_inject_init);
module_exit(nks4_inject_exit);
