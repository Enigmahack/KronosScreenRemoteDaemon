/*
 * eva_mode.c - READ-ONLY kernel memory read of Eva's live CModeManager
 * state, exposed via /proc/.eva_mode for screenremote.c to consume as the
 * primary source for its MODE=/EDITCTX= reporting (STATE, SYSINFO,
 * MODE_DETAIL). Production counterpart of ../eva_mode_peek_module/
 * eva_mode_peek.c, the diagnostic this was calibrated with - see
 * docs/EVA_ModeManager_probe.md for the full calibration session (all 7
 * SYS_MODE values and all 3 EDITCTX_RAW values independently confirmed live
 * against screenremote's own pixel ground truth, 2026-07-17) and
 * eva_mode_peek.c's header comment for the pointer-chain provenance this
 * shares verbatim (sm_poMMI -> CMMI::modeManager -> CModeManager fields).
 *
 * Trimmed from eva_mode_peek.c for production use: no DUMP hexdump (that
 * was for the Help/Compare exploratory pass, unrelated to mode detection),
 * single-line /proc output for a trivial sscanf() in screenremote.c, and
 * the field offsets are #defines exactly as calibrated - not module params,
 * same rationale as eva_mode_peek.c (a wrong offset needs re-deriving from
 * the decompile, not a live numeric tweak).
 *
 * Deliberately reports RAW SYS_MODE (0-6, Eva's own arbitrary ESysMode
 * ordinal) and RAW EDITCTX_RAW (0-2), NOT translated into screenremote's
 * public MODE=1..7/EDITCTX=0..2 wire numbering - that translation lives in
 * screenremote.c (eva_mode_read(), source/screenremote.c) so a mapping fix
 * only needs a daemon rebuild, not a kernel module rebuild+reload.
 *
 * Lowest-PID tiebreak in find_eva_mm() and the RCU-only task-list walk (no
 * tasklist_lock/get_task_struct - neither carries an EXPORT_SYMBOL on this
 * kernel) are carried over unchanged from eva_mode_peek.c; see that file's
 * header comment for the full "why" on both.
 *
 * Usage: insmod eva_mode.ko [eva_comm=Eva] [sm_pommi_addr=0x0ae431b0]
 * Then: cat /proc/.eva_mode -> "RESOLVED=1 EVA_PID=1380 SYS_MODE=0
 * EDITCTX_RAW=0 EDITCTX_SLOT=-1\n" (or "RESOLVED=0\n" if Eva isn't up yet /
 * the pointer chain doesn't resolve - not an error, just means the caller
 * should fall back to pixel detection).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/mm.h>
#include <linux/highmem.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only: exposes Eva's live CModeManager mode/edit-context state to screenremote via /proc/.eva_mode");

/* CModeManager field offsets - fully calibrated live 2026-07-17, see
 * docs/EVA_ModeManager_probe.md. Kept as #defines, not module params - see
 * eva_mode_peek.c's identical rationale. */
#define OFF_CMMI_MODEMGR   0x04UL
#define OFF_MM_SYSMODE     0x04UL
#define OFF_MM_EDITCTX     0x30UL
#define OFF_MM_EDITSLOT    0x34UL

static unsigned long sm_pommi_addr = 0x0ae431b0UL;
module_param(sm_pommi_addr, ulong, 0444);
MODULE_PARM_DESC(sm_pommi_addr, "VA of Eva's sm_poMMI global (CMMI*), default 0x0ae431b0");

static char eva_comm[TASK_COMM_LEN] = "Eva";
module_param_string(eva_comm, eva_comm, sizeof(eva_comm), 0444);
MODULE_PARM_DESC(eva_comm, "task comm name to search for (default \"Eva\")");

static struct proc_dir_entry *proc_mode;
static struct work_struct setup_work;

/* See eva_mode_peek.c's identical function for the full explanation of why
 * `current` (not Eva's task) is passed as the accounting task here. */
static int read_eva_u32(struct mm_struct *mm, unsigned long addr, u32 *out)
{
    struct page *page;
    void *kaddr;
    unsigned long off = addr & (PAGE_SIZE - 1);
    int ret;

    if (off > PAGE_SIZE - 4)
        return -EFAULT;

    down_read(&mm->mmap_sem);
    ret = get_user_pages(current, mm, addr, 1, 0, 0, &page, NULL);
    up_read(&mm->mmap_sem);

    if (ret != 1)
        return -EFAULT;

    kaddr = kmap(page);
    *out = *(u32 *)((char *)kaddr + off);
    kunmap(page);
    put_page(page);
    return 0;
}

/* See eva_mode_peek.c's identical function for the full RCU-safety and
 * lowest-PID-tiebreak rationale - carried over unchanged. */
static struct mm_struct *find_eva_mm(pid_t *out_pid)
{
    struct task_struct *p, *best = NULL;
    struct mm_struct *mm = NULL;

    rcu_read_lock();
    list_for_each_entry_rcu(p, &current->tasks, tasks) {
        if (!strncmp(p->comm, eva_comm, TASK_COMM_LEN)) {
            if (!best || p->pid < best->pid)
                best = p;
        }
    }
    if (best) {
        mm = get_task_mm(best);
        if (mm && out_pid)
            *out_pid = best->pid;
    }
    rcu_read_unlock();
    return mm;
}

static int eva_mode_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
    struct mm_struct *mm;
    pid_t eva_pid = 0;
    u32 cmmi_ptr = 0, modemgr_ptr = 0;
    u32 sys_mode = 0, editctx = 0, editslot = 0;
    int len;

    mm = find_eva_mm(&eva_pid);
    if (!mm) {
        len = snprintf(page, count, "RESOLVED=0\n");
        *eof = 1;
        return len;
    }

    if (read_eva_u32(mm, sm_pommi_addr, &cmmi_ptr) || !cmmi_ptr ||
        read_eva_u32(mm, cmmi_ptr + OFF_CMMI_MODEMGR, &modemgr_ptr) || !modemgr_ptr) {
        len = snprintf(page, count, "RESOLVED=0\n");
        goto out;
    }

    read_eva_u32(mm, modemgr_ptr + OFF_MM_SYSMODE, &sys_mode);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_EDITCTX,  &editctx);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_EDITSLOT, &editslot);

    len = snprintf(page, count,
        "RESOLVED=1 EVA_PID=%d SYS_MODE=%u EDITCTX_RAW=%u EDITCTX_SLOT=%d\n",
        eva_pid, sys_mode, editctx, (int)editslot);

out:
    mmput(mm);
    *eof = 1;
    return len;
}

static void eva_mode_setup(struct work_struct *work)
{
    /* create_proc_entry() deferral out of init_module context - see
     * CLAUDE.md's RTAI constraints table and eva_mode_peek.c/shm_peek.c,
     * which hit this failure mode first. */
    proc_mode = create_proc_entry(".eva_mode", 0444, NULL);
    if (proc_mode)
        proc_mode->read_proc = eva_mode_read_proc;
    printk(KERN_INFO "eva_mode: ready - /proc/.eva_mode (eva_comm=%s sm_pommi_addr=0x%lx)\n",
           eva_comm, sm_pommi_addr);
}

static int __init eva_mode_init(void)
{
    INIT_WORK(&setup_work, eva_mode_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit eva_mode_exit(void)
{
    flush_scheduled_work();
    if (proc_mode)
        remove_proc_entry(".eva_mode", NULL);
}

module_init(eva_mode_init);
module_exit(eva_mode_exit);
