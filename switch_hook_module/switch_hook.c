/*
 * switch_hook.c - ONE-SHOT diagnostic, READ-ONLY inline hook on
 * CSTGFrontPanel::HandleSwitchEvent(eSTGButtonCode, bool) in OA.ko, to
 * identify the Nautilus MODE/PAGE buttons' actual button codes.
 *
 * Background (2026-09-22 session): led_hook.c already confirmed, via the
 * same technique, that MODE/PAGE's LED codes are 0x000/0x001 (SetLED),
 * but that those fire only on open, never on close/dismiss - and that
 * neither SetLED16Bits nor SetLEDBlinking ever fire for them either. A
 * live attempt to actively QUERY the panel's own LED state over the wire
 * protocol (nks4_led_read.c, reg 0x51) locked up the whole unit - that
 * avenue is closed. The plan now (user's own proposal) is much simpler and
 * needs no hardware-state read at all: track a plain software toggle bit
 * per button in the daemon itself, flipped every time a real physical
 * press is observed. HandleSwitchEvent is the single dispatch point every
 * real press goes through (docs/api.md's own BUTTON command section: "the
 * same function a physical press dispatches through") - hooking it
 * read-only is the SAME safe technique as SetLED/SetLED16Bits/
 * SetLEDBlinking, which ran clean all night with zero incidents. This is
 * NOT the same risk category as nks4_led_read.c's active wire command.
 *
 * MODE/PAGE's btn-code candidates: OmapNKS4ButtonScanToFrontPanelCode
 * (OmapNKS4Module.ko's own scan-code table, extracted earlier this
 * session) revealed 18 front-panel button codes (79-94, 98-99) with no
 * entry anywhere in this daemon's existing btn_table[] (which is dense
 * 1-78, a pure Kronos-era capture) - strong candidates for Nautilus-only
 * controls. This histogram will show exactly which of those (if any) fire
 * when MODE/PAGE are physically pressed, the same before/after diff method
 * already used to find the LED codes.
 *
 * Calling convention: regparm(3), `this` in EAX (unused by the body - same
 * "blind_this()" pattern as the sibling SetLED-family methods, per
 * nks4_inject.c's own comment for this exact function), eSTGButtonCode in
 * EDX, bool pressed in ECX. This histogram only captures EDX (the code) -
 * each physical press+release cycle will show up as the SAME code counted
 * twice (once per press, once per release), which is fine for identifying
 * which code is which; a later daemon feature wanting a clean once-per-
 * press toggle would need to also branch on ECX and use only the
 * pressed==true edge.
 *
 * First 6 bytes of HandleSwitchEvent on this build (confirmed via static
 * disasm of the real Nautilus OA.ko, decrypted copy, symbol offset 0xbad0):
 * 55 89 e5 83 e4 f0 (push ebp; mov ebp,esp; and esp,0xfffffff0) - a clean
 * instruction boundary at exactly 6 bytes, same PROLOGUE_LEN as the
 * SetLED-family hooks. install_hook() verifies this exact pattern live
 * before patching anything and refuses to hook if it doesn't match.
 *
 * Usage:
 *   insmod switch_hook.ko target=0x<VA>
 *     (grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)
 *   cat /proc/.switch_hook   (snapshot: TOTAL_CALLS + every nonzero
 *                             CODE=.. COUNT=.. seen so far)
 *   Diff two snapshots around a button press to find its code.
 *   rmmod switch_hook         (always safe - restores the original 6 bytes)
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
MODULE_DESCRIPTION("ONE-SHOT diagnostic: read-only inline hook on CSTGFrontPanel::HandleSwitchEvent, histograms eSTGButtonCode by count");

static unsigned long target = 0;
module_param(target, ulong, 0444);
MODULE_PARM_DESC(target, "Live VA of CSTGFrontPanel::HandleSwitchEvent (grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)");

/* ── histogram ────────────────────────────────────────────────────────── */
/* Index = (code & 0xFF) | ((pressed & 1) << 8) - a branch-free combine of
 * both HandleSwitchEvent arguments (EDX=code, ECX=pressed) into one
 * histogram, so histogram[code] is the RELEASED count and
 * histogram[code|0x100] is the PRESSED count for that code, separately.
 * Purpose: verify pressed==1 fires exactly once per physical press (not
 * more, e.g. from debounce/repeat) before a real daemon feature bases a
 * parity toggle on it. All-bitwise, no comparisons/branches in the
 * trampoline - safer than hand-written conditional jumps. */
#define HIST_SLOTS 512
static u32 histogram[HIST_SLOTS];
static volatile u32 total_calls;

#define PROLOGUE_LEN 6
static unsigned char saved_bytes[PROLOGUE_LEN];
static unsigned char *tramp;
static int installed;
static unsigned long hooked_addr;

/*
 * Trampoline (49 bytes). EDX (code) and ECX (pressed) are read-only
 * sources but freely used as scratch material to COMPUTE eax/ebx - pushad
 * already saved their original values on the stack, and popad restores
 * them unconditionally before the saved original instructions run, so
 * clobbering edx/ecx/ebx/eax here is safe regardless of what we do with
 * them in between.
 *
 *   pushfd; pushad
 *   total_calls++
 *   eax = ecx & 1;  eax <<= 8            (pressed bit, positioned)
 *   ebx = edx & 0xFF                      (code, masked)
 *   eax |= ebx                             (combined index, 0..511)
 *   eax <<= 2;  eax += &histogram[0]        (address of the slot)
 *   histogram[index]++
 *   popad; popfd
 *   <6 saved original bytes>
 *   jmp rel32 back to target+6
 */
#define OFF_TOTAL_IMM  4    /* inc dword [total_calls]   imm32 operand */
#define OFF_HIST_IMM   30   /* add eax, histogram        imm32 operand */
#define OFF_PROLOGUE   38   /* saved original 6 bytes go here */
#define OFF_JMP_REL    45   /* jmp rel32                 imm32 operand */
#define TRAMP_SIZE     49

static const unsigned char TRAMP_TEMPLATE[TRAMP_SIZE] = {
    0x9C,                               /* pushfd */
    0x60,                               /* pushad */
    0xFF, 0x05, 0,0,0,0,                /* inc dword [total_calls] */
    0x89, 0xC8,                         /* mov eax, ecx */
    0x83, 0xE0, 0x01,                   /* and eax, 1 */
    0xC1, 0xE0, 0x08,                   /* shl eax, 8 */
    0x89, 0xD3,                         /* mov ebx, edx */
    0x81, 0xE3, 0xFF, 0x00, 0x00, 0x00, /* and ebx, 0xFF */
    0x09, 0xD8,                         /* or eax, ebx */
    0xC1, 0xE0, 0x02,                   /* shl eax, 2   (sizeof(u32)==4) */
    0x05, 0,0,0,0,                      /* add eax, histogram */
    0xFF, 0x00,                         /* inc dword [eax] */
    0x61,                               /* popad */
    0x9D,                               /* popfd */
    0,0,0,0,0,0,                        /* saved original prologue (6 bytes) */
    0xE9, 0,0,0,0,                      /* jmp rel32 back */
};

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

static int install_hook(unsigned long addr)
{
    unsigned long flags;
    unsigned char patch[PROLOGUE_LEN];
    s32 rel_back;

    memcpy(saved_bytes, (const void *)addr, PROLOGUE_LEN);

    if (saved_bytes[0] != 0x55 || saved_bytes[1] != 0x89 || saved_bytes[2] != 0xe5 ||
        saved_bytes[3] != 0x83 || saved_bytes[4] != 0xe4 || saved_bytes[5] != 0xf0) {
        printk(KERN_ERR "switch_hook: HandleSwitchEvent prologue mismatch at 0x%lx: "
               "%02x %02x %02x %02x %02x %02x (expected 55 89 e5 83 e4 f0) - "
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
    printk(KERN_INFO "switch_hook: HandleSwitchEvent hooked at 0x%lx\n", addr);
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
    printk(KERN_INFO "switch_hook: HandleSwitchEvent unhooked, original bytes restored\n");
}

/* ── /proc/.switch_hook ──────────────────────────────────────────────── */
static struct proc_dir_entry *proc_hook;
static struct work_struct setup_work;

static int switch_hook_read_proc(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
    int len = 0;
    unsigned int i;

    len += snprintf(page + len, count - len,
                     "INSTALLED=%d TARGET=0x%lx TOTAL_CALLS=%u\n",
                     installed, hooked_addr, total_calls);
    for (i = 0; i < HIST_SLOTS && len + 32 < count; i++) {
        if (histogram[i])
            len += snprintf(page + len, count - len,
                             "CODE=0x%02x PRESSED=%u COUNT=%u\n",
                             i & 0xff, (i >> 8) & 1, histogram[i]);
    }
    len += snprintf(page + len, count - len, "OK\n");

    *eof = 1;
    return len;
}

static void switch_hook_setup(struct work_struct *work)
{
    proc_hook = create_proc_entry(".switch_hook", 0444, NULL);
    if (proc_hook)
        proc_hook->read_proc = switch_hook_read_proc;
    printk(KERN_INFO "switch_hook: /proc/.switch_hook ready\n");
}

static int __init switch_hook_init(void)
{
    if (!target) {
        printk(KERN_ERR "switch_hook: target= required "
               "(grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)\n");
        return -EINVAL;
    }
    if (install_hook(target) != 0)
        return -EINVAL;

    INIT_WORK(&setup_work, switch_hook_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit switch_hook_exit(void)
{
    flush_scheduled_work();
    if (proc_hook)
        remove_proc_entry(".switch_hook", NULL);
    remove_hook();
    if (tramp)
        vfree(tramp);
}

module_init(switch_hook_init);
module_exit(switch_hook_exit);
