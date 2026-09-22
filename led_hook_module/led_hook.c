/*
 * led_hook.c - ONE-SHOT diagnostic, READ-ONLY inline hooks on
 * CSTGFrontPanel::SetLED(eSTGLEDCode) and ::SetLED16Bits(m) in OA.ko, to see
 * which LED codes/bitmasks fire live - specifically, to identify the
 * Nautilus MODE/PAGE popup-buttons' LED codes so their lit/unlit state can
 * eventually be mirrored on the client.
 *
 * Why a hook, not a memory peek: static disassembly of the real Nautilus
 * OA.ko (2026-09-22 session) showed both functions are pure fire-and-forget
 * command-senders - they pack their argument into an outbound NKS4Command
 * and transmit it, with no cached on/off state written anywhere in
 * CSTGFrontPanel's own memory. A read-only peek (the eva_mode_peek.c
 * approach, which found CModeManager has no MODE/PAGE-popup bit either) has
 * nothing to read here - the state lives on the panel MCU, not the host.
 * Capturing every live call, read-only, is the only way to see it from
 * software.
 *
 * Same base technique as KronosExtract/source/kronos_extract.c's
 * moancjsd82 hook, proven safe on real hardware: save the target's original
 * bytes, allocate an executable trampoline, patch a `jmp rel32` over the
 * original prologue, and make the trampoline log-then-fall-through to the
 * saved original code before jumping back. rmmod always restores the
 * original bytes exactly.
 *
 * TWO HOOK SITES, TWO CAPTURE STRATEGIES:
 *
 *   SetLED (single eSTGLEDCode) - a HISTOGRAM, not a ring buffer (changed
 *   after the first live test, 2026-09-22): a raw ring buffer's first live
 *   capture (RING_SLOTS=64) was completely flooded by a normal,
 *   continuously-blinking LED (code 0x0f fired ~250 times in a few seconds)
 *   before a single /proc read could catch up - any discrete MODE/PAGE-press
 *   event was long overwritten by the time `cat` ran. A count-per-code
 *   histogram (indexed directly by `code & (HIST_SLOTS-1)`, no ordering/
 *   index bookkeeping at all) can never overflow that way. Confirmed live:
 *   MODE = code 0x000, PAGE = code 0x001, both fire exactly once on open
 *   and never again on close - so whatever turns them back off does not go
 *   through SetLED.
 *
 *   SetLED16Bits (a bulk LED bitmask, presumably a full-state snapshot each
 *   call rather than a delta) - just the single LAST raw value seen plus a
 *   call counter. A histogram doesn't fit here (the argument is a wide
 *   bitmask with a huge value space, not a small enum); the intended usage
 *   is a before/after snapshot diff of last_value, bit-XORed, around a
 *   button press/dismiss - whichever bit position flips is that LED's
 *   position in the mask.
 *
 * Calling convention for BOTH (confirmed from static disasm of the real
 * Nautilus OA.ko, decrypted copy, symbol offsets 0xb810/0xb870 in its own
 * .text): regparm(3), `this` in EAX (never read - matches nks4_inject.c's
 * "blind_this()" note for this same class), the single argument in EDX.
 * SetLED's first 6 bytes are always `0f b6 c6 0f b6 d2` (movzx eax,dh;
 * movzx edx,dl) and SetLED16Bits's first 5 are always `89 d0 0f b6 ca`
 * (mov eax,edx; movzx ecx,dl) on this build - install_hook() verifies the
 * exact expected pattern live before patching either site, and refuses to
 * hook if it doesn't match (wrong OA.ko build, wrong address, etc).
 *
 * Neither hook ever writes EDX - each only reads it after pushad (which
 * doesn't disturb register contents, only saves copies on the stack), so
 * the original instructions see EDX completely unchanged after popad/popfd
 * regardless of what the logging code does with its own scratch registers.
 *
 * Usage:
 *   insmod led_hook.ko target=0x<VA> [target2=0x<VA2>]
 *     target  = grep SetLEDE11eSTGLEDCode /proc/kallsyms
 *               (the CSTGFrontPanel::SetLED line, NOT the
 *               CSTGFrontPanelMsgHandler::SetLED ones)
 *     target2 = grep SetLED16BitsEm /proc/kallsyms  [optional]
 *   cat /proc/.led_hook     (snapshot of both hooks)
 *   Diff two snapshots around a button press to find its code/bit.
 *   rmmod led_hook          (always safe - restores original bytes on
 *                            whichever site(s) were actually hooked)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <asm/cacheflush.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ONE-SHOT diagnostic: read-only inline hooks on CSTGFrontPanel::SetLED/SetLED16Bits");

static unsigned long target = 0;
module_param(target, ulong, 0444);
MODULE_PARM_DESC(target, "Live VA of CSTGFrontPanel::SetLED (grep SetLEDE11eSTGLEDCode /proc/kallsyms)");

static unsigned long target2 = 0;
module_param(target2, ulong, 0444);
MODULE_PARM_DESC(target2, "Live VA of CSTGFrontPanel::SetLED16Bits (grep SetLED16BitsEm /proc/kallsyms) - optional");

static unsigned long target3 = 0;
module_param(target3, ulong, 0444);
MODULE_PARM_DESC(target3, "Live VA of CSTGFrontPanel::SetLEDBlinking (grep SetLEDBlinkingE11eSTGLEDCode /proc/kallsyms) - optional");

/* ── CR0.WP helpers - same as kronos_extract.c, shared by both hooks ──── */
static inline unsigned long disable_wp(void)
{
    unsigned long flags, cr0;
    local_irq_save(flags);
    asm volatile("mov %%cr0,%0" : "=r"(cr0));
    asm volatile("mov %0,%%cr0" :: "r"(cr0 & ~0x10000UL));
    return flags;
}

static inline void enable_wp(unsigned long flags)
{
    unsigned long cr0;
    asm volatile("mov %%cr0,%0" : "=r"(cr0));
    asm volatile("mov %0,%%cr0" :: "r"(cr0 | 0x10000UL));
    local_irq_restore(flags);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Hook 1: SetLED(eSTGLEDCode) - histogram
 * ═══════════════════════════════════════════════════════════════════════ */

#define HIST_SLOTS 1024   /* power of 2 - see AND mask in the trampoline */
static u32 histogram[HIST_SLOTS];
static volatile u32 total_calls;

#define PROLOGUE_LEN 6
static unsigned char saved_bytes[PROLOGUE_LEN];
static unsigned char *tramp;
static int installed;
static unsigned long hooked_addr;

/*
 * Trampoline (38 bytes). EDX is read-only throughout - never clobbered -
 * so the original instructions run correctly after popad/popfd regardless.
 * Only EAX is used as scratch.
 *
 *   pushfd; pushad
 *   total_calls++
 *   eax = edx & (HIST_SLOTS-1)             (candidate LED code, masked)
 *   eax = &histogram[0] + eax*4
 *   histogram[code]++
 *   popad; popfd
 *   <6 saved original bytes: movzx eax,dh; movzx edx,dl>
 *   jmp rel32 back to target+6
 */
#define OFF_TOTAL_IMM  4    /* inc dword [total_calls]   imm32 operand */
#define OFF_HIST_IMM   19   /* add eax, histogram        imm32 operand */
#define OFF_PROLOGUE   27   /* saved original 6 bytes go here */
#define OFF_JMP_REL    34   /* jmp rel32                 imm32 operand */
#define TRAMP_SIZE     38

static const unsigned char TRAMP_TEMPLATE[TRAMP_SIZE] = {
    0x9C,                               /* pushfd */
    0x60,                               /* pushad */
    0xFF, 0x05, 0,0,0,0,                /* inc dword [total_calls] */
    0x89, 0xD0,                         /* mov eax, edx */
    0x25, (HIST_SLOTS-1) & 0xff, ((HIST_SLOTS-1) >> 8) & 0xff, 0x00, 0x00,
                                         /* and eax, HIST_SLOTS-1 */
    0xC1, 0xE0, 0x02,                   /* shl eax, 2   (sizeof(u32)==4) */
    0x05, 0,0,0,0,                      /* add eax, histogram */
    0xFF, 0x00,                         /* inc dword [eax] */
    0x61,                               /* popad */
    0x9D,                               /* popfd */
    0,0,0,0,0,0,                        /* saved original prologue (6 bytes) */
    0xE9, 0,0,0,0,                      /* jmp rel32 back */
};

static int install_hook(unsigned long addr)
{
    unsigned long flags;
    unsigned char patch[PROLOGUE_LEN];
    s32 rel_back;

    memcpy(saved_bytes, (const void *)addr, PROLOGUE_LEN);

    /* Refuse to patch anything that doesn't look exactly like SetLED's
     * known prologue on this build - movzx eax,dh ; movzx edx,dl. Wrong
     * address or a different OA.ko build must fail loudly here, not patch
     * unknown code. */
    if (saved_bytes[0] != 0x0f || saved_bytes[1] != 0xb6 || saved_bytes[2] != 0xc6 ||
        saved_bytes[3] != 0x0f || saved_bytes[4] != 0xb6 || saved_bytes[5] != 0xd2) {
        printk(KERN_ERR "led_hook: SetLED prologue mismatch at 0x%lx: "
               "%02x %02x %02x %02x %02x %02x (expected 0f b6 c6 0f b6 d2) - "
               "wrong VA or wrong OA.ko build, refusing to hook\n",
               addr, saved_bytes[0], saved_bytes[1], saved_bytes[2],
               saved_bytes[3], saved_bytes[4], saved_bytes[5]);
        return -EINVAL;
    }

    tramp = __vmalloc(TRAMP_SIZE, GFP_KERNEL, PAGE_KERNEL_EXEC);
    if (!tramp)
        return -ENOMEM;

    memcpy(tramp, TRAMP_TEMPLATE, TRAMP_SIZE);
    *(u32 *)(tramp + OFF_TOTAL_IMM) = (u32)(unsigned long)&total_calls;
    *(u32 *)(tramp + OFF_HIST_IMM)  = (u32)(unsigned long)histogram;
    memcpy(tramp + OFF_PROLOGUE, saved_bytes, PROLOGUE_LEN);

    rel_back = (s32)((addr + PROLOGUE_LEN) -
                     ((unsigned long)tramp + OFF_JMP_REL + 4));
    *(s32 *)(tramp + OFF_JMP_REL) = rel_back;
    flush_icache_range((unsigned long)tramp, (unsigned long)tramp + TRAMP_SIZE);

    patch[0] = 0xE9;
    *(s32 *)&patch[1] = (s32)((unsigned long)tramp - (addr + 5));
    patch[5] = 0x90;   /* PROLOGUE_LEN(6) > 5 - one NOP pad byte */

    flags = disable_wp();
    memcpy((void *)addr, patch, PROLOGUE_LEN);
    enable_wp(flags);
    flush_icache_range(addr, addr + PROLOGUE_LEN);

    hooked_addr = addr;
    installed = 1;
    printk(KERN_INFO "led_hook: SetLED hooked at 0x%lx\n", addr);
    return 0;
}

static void remove_hook(void)
{
    unsigned long flags;
    if (!installed)
        return;
    flags = disable_wp();
    memcpy((void *)hooked_addr, saved_bytes, PROLOGUE_LEN);
    enable_wp(flags);
    flush_icache_range(hooked_addr, hooked_addr + PROLOGUE_LEN);
    installed = 0;
    printk(KERN_INFO "led_hook: SetLED unhooked, original bytes restored\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Hook 2: SetLED16Bits(m) - last raw value + call counter
 * ═══════════════════════════════════════════════════════════════════════ */

static volatile u32 total_calls2;
static volatile u32 last_value2;

#define PROLOGUE_LEN2 5
static unsigned char saved_bytes2[PROLOGUE_LEN2];
static unsigned char *tramp2;
static int installed2;
static unsigned long hooked_addr2;

/*
 * Trampoline (26 bytes). EDX read-only throughout, no scratch register
 * needed at all - both writes are direct-to-memory.
 *
 *   pushfd; pushad
 *   total_calls2++
 *   last_value2 = edx                      (the full raw bitmask argument)
 *   popad; popfd
 *   <5 saved original bytes: mov eax,edx; movzx ecx,dl>
 *   jmp rel32 back to target2+5
 */
#define OFF_TOTAL2_IMM   4    /* inc dword [total_calls2]  imm32 operand */
#define OFF_LASTVAL_IMM  10   /* mov [last_value2], edx    imm32 operand */
#define OFF_PROLOGUE2    16   /* saved original 5 bytes go here */
#define OFF_JMP_REL2     22   /* jmp rel32                 imm32 operand */
#define TRAMP_SIZE2      26

static const unsigned char TRAMP_TEMPLATE2[TRAMP_SIZE2] = {
    0x9C,                               /* pushfd */
    0x60,                               /* pushad */
    0xFF, 0x05, 0,0,0,0,                /* inc dword [total_calls2] */
    0x89, 0x15, 0,0,0,0,                /* mov [last_value2], edx */
    0x61,                               /* popad */
    0x9D,                               /* popfd */
    0,0,0,0,0,                          /* saved original prologue (5 bytes) */
    0xE9, 0,0,0,0,                      /* jmp rel32 back */
};

static int install_hook2(unsigned long addr)
{
    unsigned long flags;
    unsigned char patch[PROLOGUE_LEN2];
    s32 rel_back;

    memcpy(saved_bytes2, (const void *)addr, PROLOGUE_LEN2);

    /* Refuse to patch anything that doesn't look exactly like
     * SetLED16Bits's known prologue on this build - mov eax,edx;
     * movzx ecx,dl. */
    if (saved_bytes2[0] != 0x89 || saved_bytes2[1] != 0xd0 ||
        saved_bytes2[2] != 0x0f || saved_bytes2[3] != 0xb6 || saved_bytes2[4] != 0xca) {
        printk(KERN_ERR "led_hook: SetLED16Bits prologue mismatch at 0x%lx: "
               "%02x %02x %02x %02x %02x (expected 89 d0 0f b6 ca) - "
               "wrong VA or wrong OA.ko build, refusing to hook\n",
               addr, saved_bytes2[0], saved_bytes2[1], saved_bytes2[2],
               saved_bytes2[3], saved_bytes2[4]);
        return -EINVAL;
    }

    tramp2 = __vmalloc(TRAMP_SIZE2, GFP_KERNEL, PAGE_KERNEL_EXEC);
    if (!tramp2)
        return -ENOMEM;

    memcpy(tramp2, TRAMP_TEMPLATE2, TRAMP_SIZE2);
    *(u32 *)(tramp2 + OFF_TOTAL2_IMM)  = (u32)(unsigned long)&total_calls2;
    *(u32 *)(tramp2 + OFF_LASTVAL_IMM) = (u32)(unsigned long)&last_value2;
    memcpy(tramp2 + OFF_PROLOGUE2, saved_bytes2, PROLOGUE_LEN2);

    rel_back = (s32)((addr + PROLOGUE_LEN2) -
                     ((unsigned long)tramp2 + OFF_JMP_REL2 + 4));
    *(s32 *)(tramp2 + OFF_JMP_REL2) = rel_back;
    flush_icache_range((unsigned long)tramp2, (unsigned long)tramp2 + TRAMP_SIZE2);

    patch[0] = 0xE9;
    *(s32 *)&patch[1] = (s32)((unsigned long)tramp2 - (addr + 5));
    /* PROLOGUE_LEN2 == 5, exactly a plain jmp rel32 - no NOP padding needed */

    flags = disable_wp();
    memcpy((void *)addr, patch, PROLOGUE_LEN2);
    enable_wp(flags);
    flush_icache_range(addr, addr + PROLOGUE_LEN2);

    hooked_addr2 = addr;
    installed2 = 1;
    printk(KERN_INFO "led_hook: SetLED16Bits hooked at 0x%lx\n", addr);
    return 0;
}

static void remove_hook2(void)
{
    unsigned long flags;
    if (!installed2)
        return;
    flags = disable_wp();
    memcpy((void *)hooked_addr2, saved_bytes2, PROLOGUE_LEN2);
    enable_wp(flags);
    flush_icache_range(hooked_addr2, hooked_addr2 + PROLOGUE_LEN2);
    installed2 = 0;
    printk(KERN_INFO "led_hook: SetLED16Bits unhooked, original bytes restored\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Hook 3: SetLEDBlinking(eSTGLEDCode) - histogram
 *
 * Byte-identical prologue to SetLED (0f b6 c6 0f b6 d2 - confirmed via
 * static disasm, symbol offset 0xb830 in the same OA.ko, differs from
 * SetLED only in a later constant, ...0x01510000 vs SetLED's 0x01500000,
 * both well past the 6-byte prologue this hook patches). Reuses
 * TRAMP_TEMPLATE/PROLOGUE_LEN from Hook 1 verbatim - just a separate
 * histogram/counter/hook-state so its results don't mix with SetLED's.
 * ═══════════════════════════════════════════════════════════════════════ */

static u32 histogram3[HIST_SLOTS];
static volatile u32 total_calls3;

static unsigned char saved_bytes3[PROLOGUE_LEN];
static unsigned char *tramp3;
static int installed3;
static unsigned long hooked_addr3;

static int install_hook3(unsigned long addr)
{
    unsigned long flags;
    unsigned char patch[PROLOGUE_LEN];
    s32 rel_back;

    memcpy(saved_bytes3, (const void *)addr, PROLOGUE_LEN);

    if (saved_bytes3[0] != 0x0f || saved_bytes3[1] != 0xb6 || saved_bytes3[2] != 0xc6 ||
        saved_bytes3[3] != 0x0f || saved_bytes3[4] != 0xb6 || saved_bytes3[5] != 0xd2) {
        printk(KERN_ERR "led_hook: SetLEDBlinking prologue mismatch at 0x%lx: "
               "%02x %02x %02x %02x %02x %02x (expected 0f b6 c6 0f b6 d2) - "
               "wrong VA or wrong OA.ko build, refusing to hook\n",
               addr, saved_bytes3[0], saved_bytes3[1], saved_bytes3[2],
               saved_bytes3[3], saved_bytes3[4], saved_bytes3[5]);
        return -EINVAL;
    }

    tramp3 = __vmalloc(TRAMP_SIZE, GFP_KERNEL, PAGE_KERNEL_EXEC);
    if (!tramp3)
        return -ENOMEM;

    memcpy(tramp3, TRAMP_TEMPLATE, TRAMP_SIZE);
    *(u32 *)(tramp3 + OFF_TOTAL_IMM) = (u32)(unsigned long)&total_calls3;
    *(u32 *)(tramp3 + OFF_HIST_IMM)  = (u32)(unsigned long)histogram3;
    memcpy(tramp3 + OFF_PROLOGUE, saved_bytes3, PROLOGUE_LEN);

    rel_back = (s32)((addr + PROLOGUE_LEN) -
                     ((unsigned long)tramp3 + OFF_JMP_REL + 4));
    *(s32 *)(tramp3 + OFF_JMP_REL) = rel_back;
    flush_icache_range((unsigned long)tramp3, (unsigned long)tramp3 + TRAMP_SIZE);

    patch[0] = 0xE9;
    *(s32 *)&patch[1] = (s32)((unsigned long)tramp3 - (addr + 5));
    patch[5] = 0x90;

    flags = disable_wp();
    memcpy((void *)addr, patch, PROLOGUE_LEN);
    enable_wp(flags);
    flush_icache_range(addr, addr + PROLOGUE_LEN);

    hooked_addr3 = addr;
    installed3 = 1;
    printk(KERN_INFO "led_hook: SetLEDBlinking hooked at 0x%lx\n", addr);
    return 0;
}

static void remove_hook3(void)
{
    unsigned long flags;
    if (!installed3)
        return;
    flags = disable_wp();
    memcpy((void *)hooked_addr3, saved_bytes3, PROLOGUE_LEN);
    enable_wp(flags);
    flush_icache_range(hooked_addr3, hooked_addr3 + PROLOGUE_LEN);
    installed3 = 0;
    printk(KERN_INFO "led_hook: SetLEDBlinking unhooked, original bytes restored\n");
}

/* ── /proc/.led_hook ──────────────────────────────────────────────────── */
static struct proc_dir_entry *proc_hook;
static struct work_struct setup_work;

static int led_hook_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
    int len = 0;
    unsigned int i;

    len += snprintf(page + len, count - len,
                     "SETLED_INSTALLED=%d SETLED_TARGET=0x%lx SETLED_TOTAL_CALLS=%u\n",
                     installed, hooked_addr, total_calls);
    for (i = 0; i < HIST_SLOTS && len + 32 < count; i++) {
        if (histogram[i])
            len += snprintf(page + len, count - len,
                             "CODE=0x%03x COUNT=%u\n", i, histogram[i]);
    }

    len += snprintf(page + len, count - len,
                     "SETLED16BITS_INSTALLED=%d SETLED16BITS_TARGET=0x%lx "
                     "SETLED16BITS_TOTAL_CALLS=%u LAST_VALUE=0x%08x\n",
                     installed2, hooked_addr2, total_calls2, last_value2);

    len += snprintf(page + len, count - len,
                     "SETLEDBLINKING_INSTALLED=%d SETLEDBLINKING_TARGET=0x%lx SETLEDBLINKING_TOTAL_CALLS=%u\n",
                     installed3, hooked_addr3, total_calls3);
    for (i = 0; i < HIST_SLOTS && len + 32 < count; i++) {
        if (histogram3[i])
            len += snprintf(page + len, count - len,
                             "BLINK_CODE=0x%03x COUNT=%u\n", i, histogram3[i]);
    }

    len += snprintf(page + len, count - len, "OK\n");

    *eof = 1;
    return len;
}

static void led_hook_setup(struct work_struct *work)
{
    /* create_proc_entry() needs deferring out of init_module context on
     * this RTAI kernel - see CLAUDE.md's "Critical RTAI constraints" table. */
    proc_hook = create_proc_entry(".led_hook", 0444, NULL);
    if (proc_hook)
        proc_hook->read_proc = led_hook_read_proc;
    printk(KERN_INFO "led_hook: /proc/.led_hook ready\n");
}

static int __init led_hook_init(void)
{
    if (!target) {
        printk(KERN_ERR "led_hook: target= required "
               "(grep SetLEDE11eSTGLEDCode /proc/kallsyms)\n");
        return -EINVAL;
    }
    if (install_hook(target) != 0)
        return -EINVAL;

    if (target2) {
        if (install_hook2(target2) != 0) {
            remove_hook();
            if (tramp) vfree(tramp);
            return -EINVAL;
        }
    }

    if (target3) {
        if (install_hook3(target3) != 0) {
            remove_hook();
            remove_hook2();
            if (tramp) vfree(tramp);
            if (tramp2) vfree(tramp2);
            return -EINVAL;
        }
    }

    INIT_WORK(&setup_work, led_hook_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit led_hook_exit(void)
{
    flush_scheduled_work();
    if (proc_hook)
        remove_proc_entry(".led_hook", NULL);
    remove_hook();
    remove_hook2();
    remove_hook3();
    if (tramp)
        vfree(tramp);
    if (tramp2)
        vfree(tramp2);
    if (tramp3)
        vfree(tramp3);
}

module_init(led_hook_init);
module_exit(led_hook_exit);
