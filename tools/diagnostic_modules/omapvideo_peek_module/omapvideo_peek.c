/*
 * omapvideo_peek.c - ONE-SHOT diagnostic, READ-ONLY kernel memory peek into
 * the already-loaded OmapVideoModule.ko's own in-memory .text/.data, since
 * no .ko file for it exists anywhere on this device's filesystem (unlike
 * Eva, it's not left on disk after loading - see
 * kronosology/docs/hardware/nautilus_color_palette.md "Round 6").
 *
 * Goal: dump small windows of OmapVideoModule's code around
 * omapfb_setcolreg/omapfb_ioctl (addresses from /proc/kallsyms) so they can
 * be disassembled on the host exactly like Eva's binary was, to find
 * whatever color-table data those functions reference - the real palette
 * consumed by the /dev/fb1 update ioctl, which Eva's own userspace code
 * never touches directly (confirmed this same investigation session).
 *
 * This is a plain kernel-space read - the target address is already mapped
 * in the kernel's own address space (a loaded module's .text/.data), so
 * unlike eva_mode_peek.c (which reads a *userspace* process's mm via
 * get_user_pages()) this just dereferences the pointer directly. No
 * hooking, no writes, nothing patched.
 *
 * Per shm_peek.c's own established lesson: ignore off/*start and write the
 * whole requested window in one read_proc call (the old kernel's
 * multi-call read_proc pagination protocol is unreliable) - so `len` here
 * must stay well under one PROC_BLOCK_SIZE (~3KB). Output is raw binary,
 * not hex, to keep within that budget for a given window.
 *
 * Usage: insmod omapvideo_peek.ko base=0x58dcd000 off=0x2f0 len=848
 * Then:  cat /proc/.omapvideo_peek > /korg/rw/ovp_dump.bin
 *        rmmod omapvideo_peek
 * (re-insmod with a different off/len for the next window)
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ONE-SHOT diagnostic: read-only peek at OmapVideoModule's own loaded memory, no hooking");

#define MAX_LEN 2800UL /* stay well under one PROC_BLOCK_SIZE read_proc call */

static unsigned long base = 0x58dcd000UL;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "OmapVideoModule load base VA (see /proc/modules)");

static unsigned long off = 0;
module_param(off, ulong, 0444);
MODULE_PARM_DESC(off, "byte offset from base to start dumping");

static unsigned long len = 512;
module_param(len, ulong, 0444);
MODULE_PARM_DESC(len, "bytes to dump, capped at MAX_LEN");

static struct proc_dir_entry *proc_peek;
static struct work_struct setup_work;

static int peek_read_proc(char *page, char **start, off_t proc_off,
                           int count, int *eof, void *data)
{
    unsigned long n = len;
    const unsigned char *src = (const unsigned char *)(base + off);

    if (n > MAX_LEN)
        n = MAX_LEN;
    if (n > (unsigned long)count)
        n = (unsigned long)count;

    memcpy(page, src, n);
    *eof = 1;
    return (int)n;
}

static void peek_setup(struct work_struct *work)
{
    /* create_proc_entry() needs deferring out of init_module context on
     * this RTAI kernel - see CLAUDE.md's "Critical RTAI constraints"
     * table. */
    proc_peek = create_proc_entry(".omapvideo_peek", 0444, NULL);
    if (proc_peek)
        proc_peek->read_proc = peek_read_proc;
    printk(KERN_INFO "omapvideo_peek: ready - cat /proc/.omapvideo_peek "
           "(base=0x%lx off=0x%lx len=%lu)\n", base, off, len);
}

static int __init peek_init(void)
{
    INIT_WORK(&setup_work, peek_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit peek_exit(void)
{
    flush_scheduled_work();
    if (proc_peek)
        remove_proc_entry(".omapvideo_peek", NULL);
}

module_init(peek_init);
module_exit(peek_exit);
