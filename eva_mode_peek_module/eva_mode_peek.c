/*
 * eva_mode_peek.c - ONE-SHOT diagnostic, READ-ONLY kernel memory peek into
 * Eva's own process memory to read its live CModeManager state directly,
 * as a candidate replacement for the framebuffer pixel-fingerprint mode
 * detection in ../source/screenremote.c (detect_ui_mode()/
 * detect_program_edit_context()).
 *
 * NOT WIRED INTO screenremote.c AND NOT YET CALIBRATED ON REAL HARDWARE -
 * see the "CALIBRATION STATUS" note below before trusting any field here.
 *
 * Background: EVA_Decomp traced CMMI::GetInstance()'s singleton (label
 * sm_poMMI, VA 0x0ae431b0 in Eva's own address space) -> +0x04 a
 * CModeManager* whose fields (ctor at CModeManager@08965310, cross-checked
 * against ChangePage@08965620 / SetEditInContext@08966740 /
 * IsOnTimbreProgramEditInContext@08966580) appear to be:
 *   +0x04  int    current ESysMode (top-level Setlist/Combi/Program/... mode)
 *   +0x20  int    target mode mid-transition
 *   +0x24  short  current sub-page id
 *   +0x28  int    previous mode
 *   +0x30  int    eSTGEditInContextType (0=none, 1=Combi - CONFIRMED via
 *                 CombiMsgHandler's IsOnTimbreProgramEditInContext call site;
 *                 2=Sequence is inferred by symmetry with SetEditInContext's
 *                 mirrored branches, NOT independently confirmed)
 *   +0x34  int    timbre/track slot being Program-edited, -1 if none
 *
 * Eva (/korg/Eva/Eva, exec'd as user pocky per kronosology/docs/workflow/
 * deploying_patches.md) is a plain ET_EXEC x86 binary, image base 0x08048000
 * (kronosology/docs/modules/Eva.md) - not PIE/ASLR'd, so 0x0ae431b0 is a
 * constant VA every boot, no per-run relocation needed, unlike OA.ko/
 * loadmod.ko's kallsyms-dependent addresses elsewhere in this project.
 *
 * This is READ-ONLY and does NOT hook or patch Eva - it locates Eva's
 * task_struct by scanning the process list for comm==eva_comm (default
 * "Eva"), pins the relevant page of its mm via get_user_pages() (the same
 * mechanism /proc/pid/mem uses), and reads through a temporary kmap().
 * Nothing about Eva's own execution is touched, same "one-shot diagnostic,
 * no hooking" bar as ../shm_peek_module/shm_peek.c and
 * ../chord_probe_module/chord_probe.c.
 *
 * CALIBRATION STATUS: FULLY CONFIRMED live on real hardware 2026-07-17 - see
 * docs/EVA_ModeManager_probe.md for the full session. Every SYS_MODE value
 * and every EDITCTX_RAW value was independently cross-checked against
 * screenremote's own pixel-based ground truth (mode_detect_refs.h scoring
 * against live REGION/PALETTE dumps) while the user stood in each state on
 * the physical unit:
 *
 *   SYS_MODE: 0=Program 1=Combi 2=Global 3=Disk 4=Sequence 5=Sampling 6=Setlist
 *   EDITCTX_RAW: 0=none 1=Program-edit-from-Combi 2=Program-edit-from-Sequence
 *
 * (Note this ordinal mapping matches NEITHER the SysEx wire numbering used
 * elsewhere in this project - KronosSysEx.cs's SysExModeData, 0=Combi
 * 2=Program etc. - NOR the client's own 1-indexed Mode enum. It's its own,
 * apparently arbitrary C++ enum declaration order - don't assume either of
 * those other numberings applies here.)
 *
 * The initial live test (2026-07-16, see git history / the doc above) DID
 * NOT match ground truth on first read - that turned out to be simply the
 * ordinal-mapping guess being wrong, not a bug in the read mechanism
 * (pointer chain and offsets were already reading real, live, correct
 * values the whole time). Confirmed via a full interactive calibration
 * pass, not yet wired into screenremote.c's MODE/EDITCTX reporting.
 *
 * Usage: insmod eva_mode_peek.ko [eva_comm=Eva] [sm_pommi_addr=0x0ae431b0]
 * Then repeatedly: cat /proc/.eva_mode_peek
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
MODULE_DESCRIPTION("ONE-SHOT diagnostic: read-only peek at Eva's CModeManager state, no hooking");

/* CModeManager field offsets - see header comment above for provenance.
 * Kept as #defines (like shm_peek.c's own calibrated-from-decompile
 * offsets) rather than module params: if these turn out wrong, the fix is
 * a source edit + rebuild, not a live insmod-time tweak, since a wrong
 * offset here means the whole field layout needs re-deriving from Eva's
 * decompile, not just a numeric nudge. */
#define OFF_CMMI_MODEMGR   0x04UL
#define OFF_MM_SYSMODE     0x04UL
#define OFF_MM_TARGETMODE  0x20UL
#define OFF_MM_SUBPAGE     0x24UL
#define OFF_MM_PREVMODE    0x28UL
#define OFF_MM_EDITCTX     0x30UL
#define OFF_MM_EDITSLOT    0x34UL

static unsigned long sm_pommi_addr = 0x0ae431b0UL;
module_param(sm_pommi_addr, ulong, 0444);
MODULE_PARM_DESC(sm_pommi_addr, "VA of Eva's sm_poMMI global (CMMI*), default 0x0ae431b0");

static char eva_comm[TASK_COMM_LEN] = "Eva";
module_param_string(eva_comm, eva_comm, sizeof(eva_comm), 0444);
MODULE_PARM_DESC(eva_comm, "task comm name to search for (default \"Eva\")");

static struct proc_dir_entry *proc_peek;
static struct work_struct setup_work;

/* Reads one u32 from `addr` inside the given mm via get_user_pages() - the
 * same mechanism /proc/pid/mem uses. Strictly read-only (write=0, force=0).
 * `current` (the reading process, e.g. `cat`) is passed as the accounting
 * task - on this kernel that argument only affects minor/major fault
 * counters, never Eva's own task_struct, so no reference to Eva's task is
 * needed here at all (see find_eva_mm()'s comment for why that matters). */
static int read_eva_u32(struct mm_struct *mm, unsigned long addr, u32 *out)
{
    struct page *page;
    void *kaddr;
    unsigned long off = addr & (PAGE_SIZE - 1);
    int ret;

    if (off > PAGE_SIZE - 4)
        return -EFAULT; /* would straddle a page boundary - none of our known offsets do */

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

/* Finds Eva by comm name and returns its mm with a safe reference (via
 * get_task_mm(), confirmed present at 0x4012a080 in a live kallsyms check),
 * entirely under rcu_read_lock() rather than tasklist_lock.
 *
 * This is a deliberate rewrite after a live insmod attempt (2026-07-16)
 * failed outright: "Unknown symbol __put_task_struct" - the original design
 * held a get_task_struct() reference across the whole read sequence, then
 * released it with put_task_struct(), whose slow path calls
 * __put_task_struct(). Checking kernel/fork.c in this exact tree confirms
 * neither __put_task_struct() nor tasklist_lock carry an EXPORT_SYMBOL -
 * both are visible in /proc/kallsyms (so their addresses are knowable) but
 * NOT linkable by an out-of-tree module the normal way, which is exactly
 * what "Unknown symbol" means (kallsyms visibility != module-exported).
 *
 * The fix avoids needing either: kernel/exit.c's __unhash_process() removes
 * a task from the `tasks` list with list_del_rcu(), and release_task() only
 * frees the struct after an RCU grace period (call_rcu(&p->rcu,
 * delayed_put_task_struct)) - so walking `tasks` under a plain
 * rcu_read_lock() (itself just preempt_disable()/enable() on this
 * non-preemptible-RCU config, no symbol needed at all) is safe on its own.
 * get_task_mm() is called while still inside that RCU section - it never
 * sleeps (task_lock() is a spinlock) - and the mm_struct it returns carries
 * its own reference (mm->mm_users), valid independent of Eva's task_struct
 * once we leave the RCU section. No task_struct reference is ever held
 * outside rcu_read_lock()/rcu_read_unlock(), so get_task_struct()/
 * put_task_struct() are never needed.
 *
 * Lowest-PID tiebreak (added 2026-07-17, see docs/EVA_ModeManager_probe.md's
 * "transient same-named process" note): a live test caught a second,
 * short-lived process also reporting comm=="Eva" mid-session - decompile
 * research afterward found several of Eva's own fork()+execl() helpers
 * (CFileOperation::TimeStamp, IsOverCurrentCondition, MountUSBDevice,
 * set_date - all periodic USB/disk housekeeping, nothing mode-related) never
 * call _exit()/exit() if the execl() fails, so a failed exec leaves a second
 * live "Eva"-comm process running ordinary Eva code until it exits on its
 * own. The real, long-lived UI process was PID-stable at 1380 across both
 * sessions; any such transient child necessarily forks *after* it, so it
 * always has a strictly higher PID. Taking the MINIMUM PID among all
 * comm==eva_comm matches (not just the first one this RCU walk happens to
 * reach) reliably selects the real Eva even if a transient collides. */
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

/* CModeManager's true size is clearly > the ~44 bytes the ctor decompile
 * suggested (we already read valid data at +0x34) - dumps CModeManager+0x00
 * through +DUMP_BYTES raw, rather than guessing more named-field offsets,
 * so a before/after diff around toggling Help/Compare (neither of which
 * maps onto any of the 6 fields calibrated 2026-07-17 - see
 * docs/EVA_ModeManager_probe.md) can spot whatever byte actually flips.
 * Best-effort: a failed read_eva_u32() just leaves that dword's bytes as
 * zero rather than aborting the whole dump. */
#define DUMP_BYTES 80
static void dump_eva_bytes(struct mm_struct *mm, unsigned long base, char *out, size_t outcap)
{
    unsigned int i;
    size_t p = 0;
    for (i = 0; i < DUMP_BYTES && p + 8 < outcap; i += 4) {
        u32 v = 0;
        read_eva_u32(mm, base + i, &v);
        p += snprintf(out + p, outcap - p, "%02x%02x%02x%02x",
                      v & 0xffU, (v >> 8) & 0xffU, (v >> 16) & 0xffU, (v >> 24) & 0xffU);
    }
}

static int eva_peek_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
    struct mm_struct *mm;
    pid_t eva_pid = 0;
    u32 cmmi_ptr = 0, modemgr_ptr = 0;
    u32 sys_mode = 0, target_mode = 0, subpage = 0, prev_mode = 0;
    u32 editctx = 0, editslot = 0;
    char dump[DUMP_BYTES * 2 + 1];
    int len;
    int rc;

    mm = find_eva_mm(&eva_pid);
    if (!mm) {
        len = snprintf(page, count,
            "RESOLVED=0\nSTAGE=find_task\nEVA_COMM=%s\n"
            "ERR=eva task not found (is Eva running? check eva_comm=)\nOK\n",
            eva_comm);
        *eof = 1;
        return len;
    }

    rc = read_eva_u32(mm, sm_pommi_addr, &cmmi_ptr);
    if (rc || !cmmi_ptr) {
        len = snprintf(page, count,
            "RESOLVED=0\nSTAGE=read_sm_pommi\nEVA_PID=%d\nSM_POMMI_ADDR=0x%08lx\nRC=%d\nOK\n",
            eva_pid, sm_pommi_addr, rc);
        goto out;
    }

    rc = read_eva_u32(mm, cmmi_ptr + OFF_CMMI_MODEMGR, &modemgr_ptr);
    if (rc || !modemgr_ptr) {
        len = snprintf(page, count,
            "RESOLVED=0\nSTAGE=read_modemgr_ptr\nEVA_PID=%d\nCMMI_PTR=0x%08x\nRC=%d\nOK\n",
            eva_pid, cmmi_ptr, rc);
        goto out;
    }

    read_eva_u32(mm, modemgr_ptr + OFF_MM_SYSMODE,    &sys_mode);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_TARGETMODE, &target_mode);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_SUBPAGE,    &subpage);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_PREVMODE,   &prev_mode);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_EDITCTX,    &editctx);
    read_eva_u32(mm, modemgr_ptr + OFF_MM_EDITSLOT,   &editslot);
    dump_eva_bytes(mm, modemgr_ptr, dump, sizeof(dump));

    len = snprintf(page, count,
        "RESOLVED=1\nEVA_PID=%d\nCMMI_PTR=0x%08x\nMODEMGR_PTR=0x%08x\n"
        "SYS_MODE=%u\nTARGET_MODE=%u\nSUBPAGE=%u\nPREV_MODE=%u\n"
        "EDITCTX_RAW=%u\nEDITCTX_SLOT=%d\nDUMP=%s\nOK\n",
        eva_pid, cmmi_ptr, modemgr_ptr,
        sys_mode, target_mode, subpage & 0xffffU, prev_mode,
        editctx, (int)editslot, dump);

out:
    mmput(mm);
    *eof = 1;
    return len;
}

static void eva_peek_setup(struct work_struct *work)
{
    /* create_proc_entry() needs deferring out of init_module context on
     * this RTAI kernel - see CLAUDE.md's "Critical RTAI constraints" table
     * and shm_peek.c, which hit exactly this failure mode first. */
    proc_peek = create_proc_entry(".eva_mode_peek", 0444, NULL);
    if (proc_peek)
        proc_peek->read_proc = eva_peek_read_proc;
    printk(KERN_INFO "eva_mode_peek: ready - cat /proc/.eva_mode_peek "
           "(eva_comm=%s sm_pommi_addr=0x%lx)\n", eva_comm, sm_pommi_addr);
}

static int __init eva_mode_peek_init(void)
{
    INIT_WORK(&setup_work, eva_peek_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit eva_mode_peek_exit(void)
{
    flush_scheduled_work();
    if (proc_peek)
        remove_proc_entry(".eva_mode_peek", NULL);
}

module_init(eva_mode_peek_init);
module_exit(eva_mode_peek_exit);
