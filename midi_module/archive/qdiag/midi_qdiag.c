/*
 * midi_qdiag.ko - READ-ONLY MIDI-out-queue diagnostic for Korg Kronos OA
 *
 * v2: ON-DEMAND /proc sampler.  The v1 design re-armed a delayed_work every
 * 500ms and printk'd ~17 lines each time.  On this RTAI box that (a) flooded
 * the VGA console synchronously and (b) monopolized the shared global events
 * workqueue that OmapNKS4/OA panel handling also use - freezing front-panel
 * input.  This version does NEITHER: it creates one /proc entry and samples
 * ONLY when that entry is read (in the reader's own process context), writing
 * results as the read OUTPUT - nothing goes to printk/dmesg/console, and no
 * timer ever occupies the workqueue.
 *
 * Usage:
 *   insmod midi_qdiag.ko regoutport=<VA of CSTGMidiPortManager::RegisterMidiOutPort>
 *   cat /proc/.midi_qdiag        # one sample per read; poll from SSH to correlate
 *   rmmod midi_qdiag
 *
 * Queue ringCtl layout (CSTGMidiQueue, confirmed from OA_real.ko):
 *   +0x08 capacity mask   +0x0c write cursor   +0x10..+0x1c reader cursor[0..3]
 *   +0x20 active reader count (byte)
 * A CSTGMidiOutPort holds 4 (queue,readerIdx) pairs from Activate():
 *   queue @ +0x08/+0x14/+0x20/+0x2c   readerIdx @ +0x10/+0x1c/+0x28/+0x34
 *
 * SAFETY: patches nothing, calls no OA method, every deref via probe_kernel_read
 * (faults clean, never oopses). create_proc_entry is deferred to a one-shot
 * worker (create_proc_entry from init_module fails under RTAI); that worker runs
 * ONCE and returns - it does not loop.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");

static unsigned long regoutport;
module_param(regoutport, ulong, 0444);

static struct work_struct setup_work;
static struct proc_dir_entry *proc_ent;
static unsigned long out_ports;     /* resolved sMidiOutPorts (array of 4 ptrs) */

static int rd32(unsigned long a, u32 *o) { return probe_kernel_read(o, (void *)a, 4); }
static int rd8 (unsigned long a, u8  *o) { return probe_kernel_read(o, (void *)a, 1); }

static int kptr_ok(unsigned long p)
{
    return p >= 0x40000000UL && p < 0xfffff000UL && (p & 3) == 0;
}

/* RegisterMidiOutPort: 0f be 50 04 / 89 04 95 <disp32=&sMidiOutPorts> / c3 */
static unsigned long resolve_out_ports(unsigned long fn)
{
    u8 sig[7];
    u32 disp;
    if (!fn || probe_kernel_read(sig, (void *)fn, 7))
        return 0;
    if (sig[0] != 0x0f || sig[1] != 0xbe || sig[2] != 0x50 || sig[3] != 0x04 ||
        sig[4] != 0x89 || sig[5] != 0x04 || sig[6] != 0x95)
        return 0;
    if (rd32(fn + 7, &disp))
        return 0;
    return (unsigned long)disp;
}

static int emit_queue(char *p, int len, int pidx, int qn, unsigned long q, unsigned long ridx_addr)
{
    u32 mask = 0, wpos = 0, cur = 0;
    u8  rcount = 0, ridx = 0;
    if (!kptr_ok(q))
        return len + sprintf(p + len, " p%d.q%d @%08lx not-active\n", pidx, qn, q);
    if (rd32(q + 0x08, &mask) || rd32(q + 0x0c, &wpos) || rd8(q + 0x20, &rcount))
        return len + sprintf(p + len, " p%d.q%d @%08lx read-fault\n", pidx, qn, q);
    rd8(ridx_addr, &ridx);
    if (ridx < 4)
        rd32(q + 0x10 + ridx * 4, &cur);
    return len + sprintf(p + len,
        " p%d.q%d @%08lx cap=%u wpos=%u readers=%u free=%d portReaderIdx=%u cursor=%u\n",
        pidx, qn, q, mask + 1, wpos, rcount, 4 - (int)rcount, ridx, cur);
}

static int qdiag_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
    int len = 0, i, found = 0;
    if (off > 0) { *eof = 1; return 0; }

    if (!out_ports)
        return sprintf(page, "sMidiOutPorts unresolved (bad regoutport)\n");

    for (i = 0; i < 4; i++) {
        u32 portp = 0, q0 = 0, q1 = 0, q2 = 0, q3 = 0;
        u8 pidx = 0xff;
        if (rd32(out_ports + i * 4, &portp) || !kptr_ok(portp))
            continue;
        found++;
        rd8(portp + 4, &pidx);
        len += sprintf(page + len, "sMidiOutPorts[%d]=%08x portIndex=%u\n", i, portp, pidx);
        rd32(portp + 0x08, &q0); len = emit_queue(page, len, i, 0, q0, portp + 0x10);
        rd32(portp + 0x14, &q1); len = emit_queue(page, len, i, 1, q1, portp + 0x1c);
        rd32(portp + 0x20, &q2); len = emit_queue(page, len, i, 2, q2, portp + 0x28);
        rd32(portp + 0x2c, &q3); len = emit_queue(page, len, i, 3, q3, portp + 0x34);
    }
    if (!found)
        len += sprintf(page + len, "no activated out-ports\n");

    /* Also show the port-manager queues the out-ports do NOT read (bulk/dump and
     * KG-realtime paths). portMgr = (port q1 ptr) - 0xc, since port q1 == portMgr+0xc. */
    {
        u32 p0v = 0, q1ptr = 0;
        if (!rd32(out_ports, &p0v) && kptr_ok(p0v) &&
            !rd32(p0v + 0x14, &q1ptr) && kptr_ok(q1ptr)) {
            unsigned long portmgr = q1ptr - 0xc;
            static const int mgroff[5] = { 0x0c, 0x70, 0xd4, 0x140, 0x1a4 };
            int k;
            for (k = 0; k < 5; k++) {
                unsigned long rctl = portmgr + mgroff[k];
                u32 m = 0, w = 0; u8 rc = 0;
                rd32(rctl + 0x08, &m); rd32(rctl + 0x0c, &w); rd8(rctl + 0x20, &rc);
                len += sprintf(page + len, " mgrq+0x%03x @%08lx cap=%u wpos=%u readers=%u\n",
                               mgroff[k], rctl, m + 1, w, rc);
            }
        }
    }
    *eof = 1;
    return len;
}

/* One-shot: resolve + create the proc entry.  Runs once, returns (no loop). */
static void qdiag_setup(struct work_struct *w)
{
    out_ports = resolve_out_ports(regoutport);
    proc_ent = create_proc_entry(".midi_qdiag", 0444, NULL);
    if (proc_ent)
        proc_ent->read_proc = qdiag_read;
    /* single KERN_INFO line, not in a loop - safe */
    printk(KERN_INFO "midi_qdiag: ready, sMidiOutPorts=0x%lx, read /proc/.midi_qdiag\n",
           out_ports);
}

static int __init midi_qdiag_init(void)
{
    INIT_WORK(&setup_work, qdiag_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit midi_qdiag_exit(void)
{
    flush_scheduled_work();
    if (proc_ent)
        remove_proc_entry(".midi_qdiag", NULL);
    printk(KERN_INFO "midi_qdiag: unloaded\n");
}

module_init(midi_qdiag_init);
module_exit(midi_qdiag_exit);
