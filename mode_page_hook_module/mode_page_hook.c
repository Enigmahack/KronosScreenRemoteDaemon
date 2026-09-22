/*
 * mode_page_hook.c - permanent, read-only inline hook on
 * CSTGFrontPanel::HandleSwitchEvent(eSTGButtonCode, bool) in OA.ko. Tracks a
 * software lit/unlit toggle for the Nautilus MODE and PAGE front-panel
 * buttons (loaded on Nautilus only - see try_load_mode_page_hook() in
 * screenremote.c). There is no host-visible copy of the real LED state
 * anywhere on this hardware, so this is a software model, not a hardware
 * reading: every observed physical press flips the corresponding bit,
 * starting from 0 (unlit) at module load, which always matches a fresh
 * boot's real state since this daemon can't run without one.
 *
 * HandleSwitchEvent is the single dispatch point every physical button
 * press (and BUTTON injection) goes through. MODE = eSTGButtonCode 1,
 * PAGE = eSTGButtonCode 2 - the same codes this daemon's btn_table[] uses
 * for COMBI/PROGRAM, an unrelated pre-existing BUTTON-injection issue on
 * Nautilus this module does not address.
 *
 * State is kept in a branch-free combined-index histogram
 * (index = (code & 0xFF) | ((pressed & 1) << 8)) inside the trampoline, so
 * the patched code path never needs a comparison or conditional jump. Only
 * two slots matter (MODE-pressed = index 0x101, PAGE-pressed = index
 * 0x102); their parity (odd/even count) is the current lit/unlit state.
 *
 * Calling convention: regparm(3), `this` in EAX (unused), eSTGButtonCode in
 * EDX, bool pressed in ECX.
 *
 * HandleSwitchEvent's first 6 bytes must be exactly
 * 55 89 e5 83 e4 f0 (push ebp; mov ebp,esp; and esp,0xfffffff0) for
 * install_hook() to patch it - a clean 6-byte instruction boundary. A
 * mismatch (wrong VA, different OA.ko build) makes the module refuse to
 * install rather than patch unknown code.
 *
 * Usage (loaded by screenremote.c - see try_load_mode_page_hook(), not
 * meant to be insmod'd by hand):
 *   insmod mode_page_hook.ko target=0x<VA>
 *     (grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)
 *   cat /proc/.mode_page_hook   -> "INSTALLED=1 MODE_LIT=0|1 PAGE_LIT=0|1\n"
 *   rmmod mode_page_hook         - restores the original bytes; not done by
 *                                  the daemon in normal operation, this
 *                                  stays loaded for the life of the process
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
MODULE_DESCRIPTION("Permanent read-only inline hook on CSTGFrontPanel::HandleSwitchEvent, tracks Nautilus MODE/PAGE LED toggle state in software");

static unsigned long target = 0;
module_param(target, ulong, 0444);
MODULE_PARM_DESC(target, "Live VA of CSTGFrontPanel::HandleSwitchEvent (grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)");

/* ── histogram ────────────────────────────────────────────────────────── */
/* Index = (code & 0xFF) | ((pressed & 1) << 8) - see header comment. Only
 * two slots matter (MODE=0x101, PAGE=0x102); the full 512-entry table
 * (2KB .bss) keeps the index computation branch-free. */
#define HIST_SLOTS 512
#define MODE_CODE 1
#define PAGE_CODE 2
#define PRESSED_BIT 0x100
static u32 histogram[HIST_SLOTS];
static volatile u32 total_calls;

#define PROLOGUE_LEN 6
static unsigned char saved_bytes[PROLOGUE_LEN];
static unsigned char *tramp;
static int installed;
static unsigned long hooked_addr;

/*
 * Trampoline (49 bytes). EDX (code) and ECX (pressed) are read-only sources
 * but freely used as scratch to compute eax/ebx - pushad already saved
 * their original values on the stack, and popad restores them
 * unconditionally before the saved original instructions run.
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
        printk(KERN_ERR "mode_page_hook: HandleSwitchEvent prologue mismatch at 0x%lx: "
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
    printk(KERN_INFO "mode_page_hook: HandleSwitchEvent hooked at 0x%lx\n", addr);
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
    printk(KERN_INFO "mode_page_hook: HandleSwitchEvent unhooked, original bytes restored\n");
}

/* ── /proc/.mode_page_hook ───────────────────────────────────────────── */
static struct proc_dir_entry *proc_entry;
static struct work_struct setup_work;

static int mode_page_hook_read_proc(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
    int len;
    u32 mode_presses = histogram[MODE_CODE | PRESSED_BIT];
    u32 page_presses = histogram[PAGE_CODE | PRESSED_BIT];

    len = snprintf(page, count,
        "INSTALLED=%d TARGET=0x%lx TOTAL_CALLS=%u "
        "MODE_LIT=%u PAGE_LIT=%u MODE_PRESSES=%u PAGE_PRESSES=%u\nOK\n",
        installed, hooked_addr, total_calls,
        mode_presses & 1U, page_presses & 1U, mode_presses, page_presses);

    *eof = 1;
    return len;
}

static void mode_page_hook_setup(struct work_struct *work)
{
    /* create_proc_entry() needs deferring out of init_module context on
     * this RTAI kernel - see CLAUDE.md's "Critical RTAI constraints" table. */
    proc_entry = create_proc_entry(".mode_page_hook", 0444, NULL);
    if (proc_entry)
        proc_entry->read_proc = mode_page_hook_read_proc;
    printk(KERN_INFO "mode_page_hook: /proc/.mode_page_hook ready\n");
}

static int __init mode_page_hook_init(void)
{
    if (!target) {
        printk(KERN_ERR "mode_page_hook: target= required "
               "(grep HandleSwitchEventE14eSTGButtonCodeb /proc/kallsyms)\n");
        return -EINVAL;
    }
    if (install_hook(target) != 0)
        return -EINVAL;

    INIT_WORK(&setup_work, mode_page_hook_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit mode_page_hook_exit(void)
{
    flush_scheduled_work();
    if (proc_entry)
        remove_proc_entry(".mode_page_hook", NULL);
    remove_hook();
    if (tramp)
        vfree(tramp);
}

module_init(mode_page_hook_init);
module_exit(mode_page_hook_exit);
