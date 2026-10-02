/*
 * eva_mode.c - READ-ONLY kernel read of Eva's live CModeManager state,
 * exposed via /proc/.eva_mode for screenremote MODE=/EDITCTX= reporting.
 * Production version of eva_mode_peek.c diagnostic module. See
 * NonCodeTechnicalInfo.md §3 for the full design.
 *
 * Differences from eva_mode_peek.c: single-line /proc output, field offsets
 * as #defines (not module params - wrong offsets need re-deriving, not tweaking).
 *
 * Reports RAW SYS_MODE (0-6) and RAW EDITCTX_RAW (0-2), not the wire-format
 * translation. Translation lives in screenremote.c so fixes need only daemon
 * rebuild, not kernel module rebuild+reload.
 *
 * Lowest-PID tiebreak in find_eva_mm() and the RCU-only task-list walk (no
 * tasklist_lock/get_task_struct - neither carries an EXPORT_SYMBOL on this
 * kernel) are carried over unchanged from eva_mode_peek.c; see that file's
 * header comment for the full "why" on both.
 *
 * Usage: insmod eva_mode.ko [eva_comm=Eva] [sm_pommi_addr=0x0ae431b0]
 * Then: cat /proc/.eva_mode -> "RESOLVED=1 EVA_PID=1380 SYS_MODE=0
 * EDITCTX_RAW=0 EDITCTX_SLOT=-1\n" (or "RESOLVED=0\nSTAGE=...\n" if Eva
 * isn't up yet / the pointer chain doesn't resolve - not an error, just
 * means the caller should fall back to pixel detection).
 *
 * STAGE= output distinguishes why RESOLVED=0: find_task (Eva not found),
 * read_sm_pommi (sm_pommi_addr doesn't resolve), or read_modemgr_ptr
 * (pointer chain broken). Without this, both "Eva hasn't started" and
 * "address needs recalibration" collapsed into bare RESOLVED=0. See
 * NonCodeTechnicalInfo.md §3 for context.
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
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/err.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only: exposes Eva's live CModeManager mode/edit-context state to screenremote via /proc/.eva_mode");

/* CModeManager field offsets - calibrated via eva_mode_peek.c diagnostic.
 * Kept as #defines, not module params (wrong offsets need recalibration). */
#define OFF_CMMI_MODEMGR   0x04UL
#define OFF_MM_SYSMODE     0x04UL
#define OFF_MM_EDITCTX     0x30UL
#define OFF_MM_EDITSLOT    0x34UL

/* 0644, not 0444: writable via sysfs. screenremote.c autodetects the real
 * value by reading Eva's ELF .symtab and live-corrects via sysfs when Eva.img
 * is mounted. Default below is Eva 3.2.2 calibration, fallback only. Re-read
 * fresh on every /proc/.eva_mode call, no caching. */
static unsigned long sm_pommi_addr = 0x0ae431b0UL;
module_param(sm_pommi_addr, ulong, 0644);
MODULE_PARM_DESC(sm_pommi_addr, "VA of Eva's sm_poMMI global (CMMI*), default 0x0ae431b0 - "
                 "live-writable via sysfs, see screenremote.c's autodetection");

static char eva_comm[TASK_COMM_LEN] = "Eva";
module_param_string(eva_comm, eva_comm, sizeof(eva_comm), 0444);
MODULE_PARM_DESC(eva_comm, "task comm name to search for (default \"Eva\")");

/* Secondary match criterion: substring in resolved exe path. Exists
 * because eva_comm alone (16-byte task name truncation) could legitimately
 * not match on a differently-named build or wrapper. Matches on eva_comm
 * OR eva_exe_path; find_eva_mm() picks lowest PID among union. */
static char eva_exe_path[64] = "/Eva/Eva";
module_param_string(eva_exe_path, eva_exe_path, sizeof(eva_exe_path), 0444);
MODULE_PARM_DESC(eva_exe_path, "substring to match against a process's resolved exe path (default \"/Eva/Eva\")");

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

/* One entry per live process with an mm - collected under RCU (see below),
 * matched against eva_comm/eva_exe_path afterward, outside RCU (matching
 * against exe_path needs mm->mmap_sem, which can sleep - not legal while
 * rcu_read_lock() is held on this kernel's non-preemptible RCU config). */
struct eva_candidate {
    pid_t            pid;
    struct mm_struct *mm;
    char             comm[TASK_COMM_LEN];
};

/* Original RCU-safety rationale (no tasklist_lock/get_task_struct - neither
 * carries an EXPORT_SYMBOL on this kernel) carried over unchanged from
 * eva_mode_peek.c - see that file's identical function for the full "why".
 *
 * Broadened 2026-07-25 (see eva_exe_path's own comment above for why):
 * every process with an mm is now a candidate, matched against EITHER
 * eva_comm OR eva_exe_path, with the lowest-PID match among all matches
 * winning - not just the first comm-based match found. get_task_mm() is
 * called under rcu_read_lock() for every candidate (matches the original
 * function's use, non-sleeping - task_lock() is a spinlock on this
 * kernel), but the actual comm/exe_path comparison happens in a second
 * pass after rcu_read_unlock(), since resolving exe_path needs
 * mm->mmap_sem (down_read can sleep) - the same down_read/up_read +
 * mm->exe_file + d_path() sequence /proc/<pid>/exe's own kernel
 * implementation uses, not a novel pattern. Every non-winning candidate's
 * mm reference is dropped via mmput() before returning; the winner's
 * reference is handed to the caller, who is already expected to mmput()
 * it (same contract as before this change). */
#define EVA_CANDIDATE_MAX 256

/* Set when find_eva_mm() aborted before scanning (candidate-array allocation
 * failed) rather than scanning and finding no Eva - see its use below. */
static int eva_scan_failed;

static struct mm_struct *find_eva_mm(pid_t *out_pid)
{
    struct task_struct *p;
    struct eva_candidate *cand;
    int ncand = 0, i, best = -1;
    struct mm_struct *result;

    if (out_pid)
        *out_pid = 0;
    cand = kmalloc(EVA_CANDIDATE_MAX * sizeof(*cand), GFP_KERNEL);
    if (!cand) {
        /* Distinguish "couldn't even look" from "looked and found nothing".
         * Both used to surface as STAGE=find_task, which on a console-less unit
         * reads as "Eva isn't running" and sends the reader down the wrong
         * diagnostic path entirely. */
        eva_scan_failed = 1;
        return NULL;
    }
    eva_scan_failed = 0;

    rcu_read_lock();
    /* for_each_process(p), not list_for_each_entry_rcu(p, &current->tasks,
     * tasks): &current->tasks is a NODE in the list, not its head, so the
     * original form enumerated every task except current and terminated
     * only by chance (encountering current's own node again). Harmless
     * today because screenremote (this module's only caller) is always a
     * thread-group leader, whose `tasks` node stays correctly linked - but
     * wrong by construction: a non-leader caller's `tasks` field holds
     * stale pointers from dup_task_struct() that were never re-linked, and
     * the walk would never re-encounter its own start node at all, saved
     * from spinning only by the incidental EVA_CANDIDATE_MAX cap below.
     * for_each_process() is init_task-anchored (EXPORT_SYMBOL'd on this
     * kernel, confirmed) and correct regardless of which task is current
     * (found 2026-09-19). */
    for_each_process(p) {
        struct mm_struct *mm;
        if (ncand >= EVA_CANDIDATE_MAX)
            break;
        mm = get_task_mm(p);
        if (!mm)
            continue;   /* no mm - kernel thread, or already exiting */
        cand[ncand].pid = p->pid;
        cand[ncand].mm  = mm;
        strncpy(cand[ncand].comm, p->comm, TASK_COMM_LEN);
        ncand++;
    }
    rcu_read_unlock();

    for (i = 0; i < ncand; i++) {
        int match = !strncmp(cand[i].comm, eva_comm, TASK_COMM_LEN);

        if (!match) {
            struct file *exe;
            down_read(&cand[i].mm->mmap_sem);
            exe = cand[i].mm->exe_file;
            if (exe) {
                char pathbuf[80];
                char *rp = d_path(&exe->f_path, pathbuf, sizeof(pathbuf));
                if (!IS_ERR(rp) && strstr(rp, eva_exe_path))
                    match = 1;
            }
            up_read(&cand[i].mm->mmap_sem);
        }
        if (match && (best < 0 || cand[i].pid < cand[best].pid))
            best = i;
    }

    for (i = 0; i < ncand; i++) {
        if (i != best)
            mmput(cand[i].mm);
    }

    result = (best >= 0) ? cand[best].mm : NULL;
    if (result && out_pid)
        *out_pid = cand[best].pid;
    kfree(cand);
    return result;
}

static int eva_mode_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
    struct mm_struct *mm;
    pid_t eva_pid = 0;
    u32 cmmi_ptr = 0, modemgr_ptr = 0;
    u32 sys_mode = 0, editctx = 0, editslot = 0;
    int len, rc;

    mm = find_eva_mm(&eva_pid);
    if (!mm) {
        /* No process named eva_comm exists right now - screenremote.c's own
         * independent find_eva_pid()/eva_uptime_seconds() fallback (see its
         * update_boot_state()) does the identical /proc scan, so if THAT is
         * also never finding Eva, the boot gate's 180s uptime override can
         * never fire either - both signals starve together. STAGE=find_task
         * distinguishes this from a resolved-but-wrong-address failure below,
         * which used to be indistinguishable from this one (both just printed
         * RESOLVED=0) - see docs/EVA_ModeManager_probe.md's STAGE= field on
         * eva_mode_peek.c, the diagnostic this was trimmed from originally. */
        len = snprintf(page, count, "RESOLVED=0\nSTAGE=%s\n",
                       eva_scan_failed ? "alloc" : "find_task");
        *eof = 1;
        return len;
    }

    rc = read_eva_u32(mm, sm_pommi_addr, &cmmi_ptr);
    if (rc || !cmmi_ptr) {
        len = snprintf(page, count,
            "RESOLVED=0\nSTAGE=read_sm_pommi\nEVA_PID=%d\nSM_POMMI_ADDR=0x%08lx\nRC=%d\n",
            eva_pid, sm_pommi_addr, rc);
        goto out;
    }

    rc = read_eva_u32(mm, cmmi_ptr + OFF_CMMI_MODEMGR, &modemgr_ptr);
    if (rc || !modemgr_ptr) {
        len = snprintf(page, count,
            "RESOLVED=0\nSTAGE=read_modemgr_ptr\nEVA_PID=%d\nCMMI_PTR=0x%08x\nRC=%d\n",
            eva_pid, cmmi_ptr, rc);
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
