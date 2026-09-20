/*
 * nks4_colorpal_capture.c - inline hook on OmapNKS4's exported
 * OmapNKS4UpdateColorPal(index, R, G, B), read-only capture, to see every
 * live call while the module is loaded - triggered by the user forcing a
 * screen repaint on the physical Nautilus.
 *
 * CONFIG_KPROBES is NOT set in this kernel build (checked directly against
 * /home/build/linux-kronos/.config this session), so this uses the same
 * manual inline-detour technique KronosExtract/source/kronos_extract.c
 * already uses successfully in this project (disable_wp/__vmalloc(...,
 * PAGE_KERNEL_EXEC)/flush_icache_range), just with a purpose-built
 * trampoline for this function's specific regparm(3) calling convention
 * instead of kronos_extract's own moancjsd82-specific one.
 *
 * Prologue confirmed LIVE this session (not assumed from an old note):
 *   55 89 e5 83 e4 f0 57 89 d7 56 89 ce 53 ...
 *   push ebp; mov ebp,esp; and esp,-16; push edi; mov edi,edx; push esi;
 *   mov esi,ecx; push ebx; ...
 * `and esp,-16` spans bytes 3-5, so a plain 5-byte E9 patch would land
 * mid-instruction - PROBE_LEN=6 (5-byte jmp + 1 filler NOP) is the clean
 * boundary, matching kronosology's earlier "Round 1" finding for this same
 * function on a different unit.
 *
 * Calling convention (regparm(3), also confirmed live via the same
 * disassembly this session traced OmapNKS4UpdateColorPal's caller through):
 *   EAX = index, EDX = R, ECX = G, B spilled to the caller's stack at
 *   [orig_esp+4] (one slot past the return address).
 *
 * Trampoline: pushfd/pushad (preserves everything - the real function body
 * we jump back into still needs the original EAX/EDX/ECX), stash B from
 * the stack into a global, restore EAX from the pushad-saved copy (the
 * only register our stash step clobbers), call a plain regparm(3) C
 * function with (index,R,G) in EAX/EDX/ECX exactly as GCC expects, popad/
 * popfd, re-execute the 6 saved prologue bytes, jump back to
 * addr+PROBE_LEN. Read-only: capture_hit() only appends to an in-memory
 * log, never writes back to any hooked function's state.
 *
 * Output: /korg/rw/nks4_colorpal_capture.log - each call appended as one
 * {seq:u32, index:u8, r:u8, g:u8, b:u8, pad:u8[3]} 8-byte record, in call
 * order (not just a final snapshot).
 *
 * Deploy:
 *   insmod nks4_colorpal_capture.ko
 *   [trigger a screen repaint on the physical Nautilus]
 *   rmmod nks4_colorpal_capture
 *   scp off /korg/rw/nks4_colorpal_capture.log
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <asm/cacheflush.h>

#define OUTPUT_PATH "/korg/rw/nks4_colorpal_capture.log"
#define MAX_RECORDS 4096
#define PROBE_LEN 6

/* base = OmapNKS4's load base (see /proc/modules), off = confirmed-live
 * offset of OmapNKS4UpdateColorPal within it this session. Passed as
 * params instead of resolving via kallsyms_lookup_name(), which isn't
 * itself guaranteed EXPORT_SYMBOL'd on this kernel (kallsyms-visible !=
 * module-linkable - the exact gotcha eva_mode_peek.c's own header already
 * documents for this project). */
static unsigned long base = 0x58da4000UL;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "OmapNKS4 load base VA (see /proc/modules)");

static unsigned long off = 0x8080UL;
module_param(off, ulong, 0444);
MODULE_PARM_DESC(off, "byte offset of OmapNKS4UpdateColorPal within base");

struct record { u32 seq; u8 index; u8 r; u8 g; u8 b; u8 pad[3]; };

static struct record log_buf[MAX_RECORDS];
static atomic_t rec_count = ATOMIC_INIT(0);
static struct work_struct write_work;

static volatile u32 g_b_stash;

static unsigned long target_addr;
static unsigned char saved_bytes[PROBE_LEN];
static void *tramp;
static int installed;

#define TRAMP_SIZE 33
#define OFF_GBSTASH_IMM 7
#define OFF_CALL_REL    16
#define OFF_PROLOGUE    22
#define OFF_JMP_REL     29

/* Read-only: only appends to log_buf. Called with the real function's
 * original EAX/EDX/ECX exactly as its caller set them (regparm(3)). */
__attribute__((regparm(3)))
static void capture_hit(u8 index, u8 r, u8 g)
{
    u8 b = (u8)g_b_stash;
    int n = atomic_inc_return(&rec_count) - 1;

    if (n >= 0 && n < MAX_RECORDS) {
        log_buf[n].seq = (u32)n;
        log_buf[n].index = index;
        log_buf[n].r = r;
        log_buf[n].g = g;
        log_buf[n].b = b;
    }
    if (n == MAX_RECORDS - 1)
        schedule_work(&write_work);
}

static void do_write(struct work_struct *unused)
{
    struct file *f;
    mm_segment_t old_fs;
    loff_t pos = 0;
    int n = atomic_read(&rec_count);

    if (n > MAX_RECORDS)
        n = MAX_RECORDS;

    old_fs = get_fs();
    set_fs(KERNEL_DS);
    f = filp_open(OUTPUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!IS_ERR(f)) {
        vfs_write(f, (const char *)log_buf, n * sizeof(struct record), &pos);
        filp_close(f, NULL);
        printk(KERN_INFO "nks4_colorpal_capture: wrote %d records to %s\n",
               n, OUTPUT_PATH);
    } else {
        printk(KERN_ERR "nks4_colorpal_capture: cannot open %s (err %ld)\n",
               OUTPUT_PATH, PTR_ERR(f));
    }
    set_fs(old_fs);
}

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

static int install_hook(void)
{
    unsigned char *t;
    unsigned char patch[PROBE_LEN];
    unsigned long flags;
    s32 rel;

    target_addr = base + off;

    memcpy(saved_bytes, (const void *)target_addr, PROBE_LEN);
    printk(KERN_INFO "nks4_colorpal_capture: target @ 0x%lx  %02x%02x%02x%02x%02x%02x\n",
           target_addr,
           saved_bytes[0], saved_bytes[1], saved_bytes[2],
           saved_bytes[3], saved_bytes[4], saved_bytes[5]);

    /* Sanity-check the prologue we expect (see file header) before
     * patching anything - refuse rather than guess if it doesn't match. */
    if (!(saved_bytes[0] == 0x55 && saved_bytes[1] == 0x89 &&
          saved_bytes[2] == 0xe5 && saved_bytes[3] == 0x83 &&
          saved_bytes[4] == 0xe4 && saved_bytes[5] == 0xf0)) {
        printk(KERN_ERR "nks4_colorpal_capture: prologue mismatch - "
               "refusing to patch (build differs from what this module "
               "was written against)\n");
        return -EINVAL;
    }

    tramp = __vmalloc(TRAMP_SIZE, GFP_KERNEL, PAGE_KERNEL_EXEC);
    if (!tramp)
        return -ENOMEM;

    t = (unsigned char *)tramp;
    t[0] = 0x9c;                         /* pushfd */
    t[1] = 0x60;                         /* pushad */
    t[2] = 0x8b; t[3] = 0x44; t[4] = 0x24; t[5] = 0x28; /* mov eax,[esp+0x28] */
    t[6] = 0xa3;                         /* mov [imm32],eax */
    *(u32 *)(t + OFF_GBSTASH_IMM) = (u32)(unsigned long)&g_b_stash;
    t[11] = 0x8b; t[12] = 0x44; t[13] = 0x24; t[14] = 0x1c; /* mov eax,[esp+0x1c] */
    t[15] = 0xe8;                        /* call rel32 */
    rel = (s32)((unsigned long)capture_hit -
                 ((unsigned long)tramp + OFF_CALL_REL + 4));
    *(s32 *)(t + OFF_CALL_REL) = rel;
    t[20] = 0x61;                        /* popad */
    t[21] = 0x9d;                        /* popfd */
    memcpy(t + OFF_PROLOGUE, saved_bytes, PROBE_LEN);
    t[28] = 0xe9;                        /* jmp rel32 */
    rel = (s32)((target_addr + PROBE_LEN) -
                 ((unsigned long)tramp + OFF_JMP_REL + 4));
    *(s32 *)(t + OFF_JMP_REL) = rel;

    flush_icache_range((unsigned long)tramp, (unsigned long)tramp + TRAMP_SIZE);

    patch[0] = 0xe9;
    *(s32 *)&patch[1] = (s32)((unsigned long)tramp - (target_addr + 5));
    patch[5] = 0x90; /* NOP filler for the 6th byte */

    flags = disable_wp();
    memcpy((void *)target_addr, patch, PROBE_LEN);
    enable_wp(flags);
    flush_icache_range(target_addr, target_addr + PROBE_LEN);

    installed = 1;
    printk(KERN_INFO "nks4_colorpal_capture: hook installed, trampoline @ %p\n",
           tramp);
    return 0;
}

static void remove_hook(void)
{
    unsigned long flags;

    if (!installed)
        return;
    flags = disable_wp();
    memcpy((void *)target_addr, saved_bytes, PROBE_LEN);
    enable_wp(flags);
    flush_icache_range(target_addr, target_addr + PROBE_LEN);
    installed = 0;
    printk(KERN_INFO "nks4_colorpal_capture: hook removed, original bytes restored\n");
}

static int __init capture_init(void)
{
    int ret;

    INIT_WORK(&write_work, do_write);
    ret = install_hook();
    if (ret)
        return ret;
    printk(KERN_INFO "nks4_colorpal_capture: armed - trigger a screen "
           "repaint on the physical Nautilus now\n");
    return 0;
}

static void __exit capture_exit(void)
{
    remove_hook();
    cancel_work_sync(&write_work);
    do_write(NULL);
    if (tramp)
        vfree(tramp);
    printk(KERN_INFO "nks4_colorpal_capture: unloaded (%d calls captured)\n",
           atomic_read(&rec_count));
}

module_init(capture_init);
module_exit(capture_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Live inline-hook capture of OmapNKS4UpdateColorPal calls, read-only, auto-restoring");
