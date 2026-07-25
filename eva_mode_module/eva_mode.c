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
 * EDITCTX_RAW=0 EDITCTX_SLOT=-1\n" (or "RESOLVED=0\nSTAGE=...\n" if Eva
 * isn't up yet / the pointer chain doesn't resolve - not an error, just
 * means the caller should fall back to pixel detection).
 *
 * STAGE= (added after a real console-less production unit sat with
 * RESOLVED=0 for 30+ minutes with no way to tell why - see
 * docs/EVA_ModeManager_probe.md): which hop failed, same three stages
 * eva_mode_peek.c's diagnostic STAGE= field already distinguished -
 * find_task (no process named eva_comm - the same condition
 * screenremote.c's own independent find_eva_pid() fallback needs to see
 * Eva to ever clear the boot gate via EVA_BOOT_UPTIME_OVERRIDE_S), or
 * read_sm_pommi/read_modemgr_ptr (Eva process found, but sm_pommi_addr or
 * its +OFF_CMMI_MODEMGR hop doesn't hold the expected pointer for this
 * Eva build - address recalibration needed, not a "Eva hasn't started
 * yet" condition). Previously both collapsed into a bare "RESOLVED=0\n",
 * indistinguishable without swapping in eva_mode_peek.c and reading it
 * interactively - not possible on a unit with no console/dropbear.
 * screenremote.c's own eva_mode_read() is unaffected: it already stops
 * scanning at "RESOLVED=%d" failing to reach 5 sscanf'd fields, so the
 * extra STAGE=/... lines after a RESOLVED=0 are inert to it.
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

/* CModeManager field offsets - fully calibrated live 2026-07-17, see
 * docs/EVA_ModeManager_probe.md. Kept as #defines, not module params - see
 * eva_mode_peek.c's identical rationale. */
#define OFF_CMMI_MODEMGR   0x04UL
#define OFF_MM_SYSMODE     0x04UL
#define OFF_MM_EDITCTX     0x30UL
#define OFF_MM_EDITSLOT    0x34UL

/* 0644, not 0444: screenremote.c's resolve_eva_sm_pommi_addr() (source/
 * screenremote.c) autodetects the real value for whatever Eva build is
 * actually running - by reading CMMI::sm_poMMI straight out of Eva's own
 * on-disk ELF .symtab, since the shipped binary isn't stripped - and
 * live-corrects this via /sys/module/eva_mode/parameters/sm_pommi_addr the
 * moment that becomes possible (Eva.img's cryptoloop mount has to exist
 * first). The compiled-in default below is calibrated against the 3.2.2
 * Eva build only (see docs/EVA_ModeManager_probe.md) and is a fallback for
 * if that autodetection never manages to run this boot, not the expected
 * steady-state value on every unit. eva_mode_read_proc() re-reads this
 * global fresh on every /proc/.eva_mode call (no caching), so a sysfs write
 * takes effect on the very next read, no module reload required. */
static unsigned long sm_pommi_addr = 0x0ae431b0UL;
module_param(sm_pommi_addr, ulong, 0644);
MODULE_PARM_DESC(sm_pommi_addr, "VA of Eva's sm_poMMI global (CMMI*), default 0x0ae431b0 - "
                 "live-writable via sysfs, see screenremote.c's autodetection");

static char eva_comm[TASK_COMM_LEN] = "Eva";
module_param_string(eva_comm, eva_comm, sizeof(eva_comm), 0444);
MODULE_PARM_DESC(eva_comm, "task comm name to search for (default \"Eva\")");

/* Second, independent match criterion alongside eva_comm - a substring to
 * look for in a candidate process's resolved exe path (via mm->exe_file +
 * d_path(), the same mechanism /proc/<pid>/exe uses - both exported/
 * directly-accessible on this kernel, no tasklist_lock/get_task_struct
 * trap like find_eva_mm()'s own header comment warns about elsewhere).
 * Exists because eva_comm alone is one hardcoded 16-byte task name (the
 * TASK_COMM_LEN truncation of argv[0]) that a different Eva build, a
 * differently-named wrapper/supervisor process, or any future OS revision
 * could legitimately not match, with nothing to fall back on - see
 * docs/EVA_ModeManager_probe.md's real-hardware incident where RESOLVED=0
 * held for 30+ minutes with STAGE=find_task and no way to tell whether
 * that meant "Eva isn't running" or "Eva IS running under a name we
 * didn't anticipate". A process matching either eva_comm OR eva_exe_path
 * is now a candidate; find_eva_mm() picks the lowest PID among the union,
 * the same tiebreak already used for the transient-same-comm-process case
 * (see below) - a transient process could equally share a exe path
 * (e.g. a re-exec of the same binary) as share a comm. */
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

static struct mm_struct *find_eva_mm(pid_t *out_pid)
{
    struct task_struct *p;
    struct eva_candidate *cand;
    int ncand = 0, i, best = -1;
    struct mm_struct *result;

    cand = kmalloc(EVA_CANDIDATE_MAX * sizeof(*cand), GFP_KERNEL);
    if (!cand)
        return NULL;   /* extremely unlikely - caller treats as unresolved, same as any other miss */

    rcu_read_lock();
    list_for_each_entry_rcu(p, &current->tasks, tasks) {
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
        len = snprintf(page, count, "RESOLVED=0\nSTAGE=find_task\n");
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
