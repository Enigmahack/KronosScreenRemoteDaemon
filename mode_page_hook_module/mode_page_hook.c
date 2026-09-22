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
 * Patch site: HandleSwitchEvent+0x0a, a single 5-byte `mov eax,[abs32]`
 * (a1 imm32, the CPowerOffTimer::sInstance load) following a 10-byte
 * prologue that touches neither EDX nor ECX. Replacing exactly one whole
 * instruction with an equal-length `jmp rel32` means no instruction
 * boundary moves: a task preempted anywhere in the function (the kernel is
 * CONFIG_PREEMPT) resumes on a valid instruction whether it stopped before
 * or after the patch. Patching the 3-instruction prologue instead would
 * leave a task preempted at +1/+3 resuming in the middle of the jmp.
 *
 * The 5 patched bytes lie inside one naturally aligned qword (the function
 * is 16-byte aligned, so +0x08..+0x0f), and both install and removal
 * rewrite that qword with a single `lock cmpxchg8b`. Every other CPU (Linux
 * or RTAI domain, which a CPU rendezvous such as stop_machine() cannot
 * stop) therefore fetches either the complete original instruction or the
 * complete jmp, never a jmp with a partially written target. The bytes
 * before the patch site (55 89 e5 83 e4 f0 8d 64 24 f0 a1) are verified
 * first; a mismatch (wrong VA, different OA.ko build) or an unaligned
 * target makes the module refuse to install rather than patch unknown code.
 *
 * The trampoline, its counters and histogram share one vmalloc block that
 * is intentionally never freed once the hook has been live: a task
 * preempted inside the trampoline when the module is unloaded must still
 * find valid code and counters when it resumes.
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
 * (2KB) keeps the index computation branch-free. */
#define HIST_SLOTS 512
#define MODE_CODE 1
#define PAGE_CODE 2
#define PRESSED_BIT 0x100

/* ── patch site ─────────────────────────────────────────────────────── */
#define PATCH_OFF      0x0a  /* HandleSwitchEvent+0x0a: mov eax,[abs32] */
#define PATCH_LEN      5
#define QWORD_OFF      0x08  /* aligned qword holding bytes +0x08..+0x0f */
#define QWORD_POS      (PATCH_OFF - QWORD_OFF)

/* HandleSwitchEvent+0x00..+0x0a, up to and including the patched
 * instruction's a1 opcode (its imm32 is relocated, so not compared). */
static const unsigned char EXPECTED[PATCH_OFF + 1] = {
    0x55,                   /* push ebp */
    0x89, 0xe5,             /* mov ebp, esp */
    0x83, 0xe4, 0xf0,       /* and esp, 0xfffffff0 */
    0x8d, 0x64, 0x24, 0xf0, /* lea esp, [esp-0x10] */
    0xa1,                   /* mov eax, [abs32] */
};

/*
 * Trampoline (48 bytes). EDX (code) and ECX (pressed) are read-only sources
 * but freely used as scratch to compute eax/ebx - pushad already saved
 * their original values on the stack, and popad restores them
 * unconditionally before the relocated original instruction runs. That
 * instruction's operand is an absolute address, so it runs unchanged here.
 */
#define OFF_TOTAL_IMM  4    /* inc dword [total_calls]   imm32 operand */
#define OFF_HIST_IMM   30   /* add eax, histogram        imm32 operand */
#define OFF_ORIG       38   /* original 5-byte instruction goes here */
#define OFF_JMP_REL    44   /* jmp rel32                 imm32 operand */
#define TRAMP_SIZE     48

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
    0,0,0,0,0,                          /* original mov eax,[abs32] (5 bytes) */
    0xE9, 0,0,0,0,                      /* jmp rel32 back */
};

/* Everything the patched code path touches lives here, in one executable
 * vmalloc block that outlives the module - see header comment. */
struct hook_block {
    unsigned char code[64];
    volatile u32 total_calls;
    u32 histogram[HIST_SLOTS];
};

static struct hook_block *blk;
static int installed;
static unsigned long hooked_addr;
static u64 orig_qword, patched_qword;

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

/* Single locked 8-byte store: other CPUs observe the old or the new qword,
 * never a mix. Returns the value found at *p (== expected on success). */
static u64 swap_code_qword(u64 *p, u64 expected, u64 new)
{
    unsigned long flags;
    u64 prev;

    flags = disable_wp();
    asm volatile("lock; cmpxchg8b %1"
                 : "=A"(prev), "+m"(*p)
                 : "b"((u32)new), "c"((u32)(new >> 32)), "0"(expected)
                 : "memory");
    enable_wp(flags);
    return prev;
}

static int install_hook(unsigned long addr)
{
    unsigned long site = addr + PATCH_OFF;
    u64 *qp = (u64 *)(addr + QWORD_OFF);
    unsigned char *pb = (unsigned char *)&patched_qword;
    unsigned char *code;
    const unsigned char *live = (const unsigned char *)addr;
    s32 rel_back;
    int i;

    if (addr & 7) {
        printk(KERN_ERR "mode_page_hook: HandleSwitchEvent at 0x%lx is not 8-byte "
               "aligned - patch would straddle two qwords, refusing to hook\n", addr);
        return -EINVAL;
    }

    for (i = 0; i < (int)sizeof(EXPECTED); i++) {
        if (live[i] != EXPECTED[i]) {
            printk(KERN_ERR "mode_page_hook: HandleSwitchEvent byte mismatch at "
                   "0x%lx+0x%x: %02x (expected %02x) - wrong VA or wrong OA.ko "
                   "build, refusing to hook\n", addr, i, live[i], EXPECTED[i]);
            return -EINVAL;
        }
    }

    blk = __vmalloc(sizeof(*blk), GFP_KERNEL | __GFP_ZERO, PAGE_KERNEL_EXEC);
    if (!blk)
        return -ENOMEM;
    code = blk->code;

    orig_qword = *qp;

    memcpy(code, TRAMP_TEMPLATE, TRAMP_SIZE);
    *(u32 *)(code + OFF_TOTAL_IMM) = (u32)(unsigned long)&blk->total_calls;
    *(u32 *)(code + OFF_HIST_IMM)  = (u32)(unsigned long)blk->histogram;
    memcpy(code + OFF_ORIG, (const unsigned char *)&orig_qword + QWORD_POS, PATCH_LEN);

    rel_back = (s32)((site + PATCH_LEN) -
                     ((unsigned long)code + OFF_JMP_REL + 4));
    *(s32 *)(code + OFF_JMP_REL) = rel_back;
    flush_icache_range((unsigned long)code, (unsigned long)code + TRAMP_SIZE);

    patched_qword = orig_qword;
    pb[QWORD_POS] = 0xE9;
    *(s32 *)&pb[QWORD_POS + 1] = (s32)((unsigned long)code - (site + 5));

    if (swap_code_qword(qp, orig_qword, patched_qword) != orig_qword) {
        printk(KERN_ERR "mode_page_hook: HandleSwitchEvent+0x%x changed during "
               "install, refusing to hook\n", QWORD_OFF);
        vfree(blk);
        blk = NULL;
        return -EBUSY;
    }
    flush_icache_range(addr + QWORD_OFF, addr + QWORD_OFF + 8);

    hooked_addr = addr;
    installed = 1;
    printk(KERN_INFO "mode_page_hook: HandleSwitchEvent+0x%x hooked at 0x%lx\n",
           PATCH_OFF, addr);
    return 0;
}

/* The hook block is deliberately not freed: a task preempted inside the
 * trampoline must still find valid code and counters when it resumes. */
static void remove_hook(void)
{
    u64 *qp;
    if (!installed)
        return;
    qp = (u64 *)(hooked_addr + QWORD_OFF);
    if (swap_code_qword(qp, patched_qword, orig_qword) != patched_qword) {
        printk(KERN_ERR "mode_page_hook: HandleSwitchEvent+0x%x no longer holds "
               "our jmp - leaving it untouched\n", QWORD_OFF);
        return;
    }
    flush_icache_range(hooked_addr + QWORD_OFF, hooked_addr + QWORD_OFF + 8);
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
    u32 mode_presses = blk->histogram[MODE_CODE | PRESSED_BIT];
    u32 page_presses = blk->histogram[PAGE_CODE | PRESSED_BIT];

    len = snprintf(page, count,
        "INSTALLED=%d TARGET=0x%lx TOTAL_CALLS=%u "
        "MODE_LIT=%u PAGE_LIT=%u MODE_PRESSES=%u PAGE_PRESSES=%u\nOK\n",
        installed, hooked_addr, blk->total_calls,
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
}

module_init(mode_page_hook_init);
module_exit(mode_page_hook_exit);
