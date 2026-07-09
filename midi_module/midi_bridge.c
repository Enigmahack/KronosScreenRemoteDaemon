/*
 * midi_bridge.ko - MIDI injection + output capture for Korg Kronos
 *
 * Successor to midi_inject.ko. Same two /proc surfaces, but MIDI-OUT capture no
 * longer patches OA .text:
 *
 *   /proc/.midi_in    - write raw MIDI bytes to inject into OA.ko MIDI IN
 *   /proc/.midi_ring  - read the Kronos MIDI OUT stream (notes, CC, bend, SysEx)
 *   /proc/.midi_ports - diagnostic: out-queue tap state
 *
 * MIDI IN (unchanged from midi_inject): resolve sMidiInPorts via the byte pattern
 * in RegisterMidiInPort, then call the real MidiInPortGeneric7Receive on the active
 * port object. No .text patch.
 *
 * MIDI OUT (new, hook-free): each CSTGMidiOutPort transmits the merge of several
 * lock-free multi-reader ring queues (CSTGMidiQueue); a reader is claimed by an
 * atomic increment of the reader-count byte at ringCtl+0x20 (OA's AllocReader).
 * Instead of trampoline-hooking CSTGMidiOutPort::ReadNextMessage, we claim our OWN
 * spare reader slots on OA's source queues and drain them. We tap generically: the
 * SHARED performance queues (notes/CC/PC/combi SysEx - identical across out-ports,
 * tapped once) plus EVERY out-port's per-port queue, where bulk data dumps route
 * per destination (USB ~1 MB/s, DIN ~3.6 KB/s). This gives one destination-agnostic
 * stream: performance appears once (per-port queues don't echo it - verified) and a
 * dump to ANY destination is captured. q0 (active-sensing) is excluded.
 *
 * On-hardware measurement (2026-07-09) and a full trace of the OA out-port
 * construction proved OA registers exactly 2 out-ports (fixed at compile time in
 * CKorgUsbAudioDriverMidiPorts's static init), so each queue's reader count is a
 * stable 2 with free slots. Because OA never grows the count at runtime, we are
 * always the top reader on each queue, so on unload we atomically give the slots
 * back (no leak, clean reload). See project-midi-out-queue-tap-feasibility.
 *
 * The drain runs when /proc/.midi_ring is READ (in the reader's process context),
 * not on a timer or the shared workqueue and with no console output - a read-only
 * predecessor diagnostic froze the front panel by doing exactly those things.
 *
 * Flow control: an added reader participates in the ring's drop-on-full free-space
 * calc, so a slow consumer could otherwise throttle OA's real DIN/USB output. The
 * drain is therefore best-effort: if we fall behind, we skip our cursor forward
 * rather than hold it back, dropping our copy instead of stalling OA.
 *
 * Module params:
 *   receive_fn=0x...   MidiInPortGeneric7Receive (MIDI IN injection)
 *   register_fn=0x...  RegisterMidiInPort (MIDI IN port discovery)
 *   regoutport=0x...   CSTGMidiPortManager::RegisterMidiOutPort (MIDI OUT tap)
 *                      grep RegisterMidiOutPort /proc/kallsyms
 * The tap is generic: it captures the shared performance queues plus every
 * out-port's per-port dump queue, so one destination-agnostic stream carries
 * everything the Kronos transmits (see tap_claim_reader).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */
/*  Module parameters                                                  */
/* ------------------------------------------------------------------ */

static unsigned long receive_fn = 0;
module_param(receive_fn, ulong, 0444);

static unsigned long register_fn = 0;
module_param(register_fn, ulong, 0444);

static unsigned long regoutport = 0;
module_param(regoutport, ulong, 0444);


/* Port layout from CSTGMidiOutPort::Activate: queue ptr @ port+0x08/+0x14/+0x20/
 * +0x2c, data buffer @ port+0x0c/+0x18/+0x24/+0x30 (reader-index bytes at
 * +0x10/+0x1c/+0x28/+0x34 are the PORT's own readers, not ours). */
static const int QUEUE_PTR_OFF[4] = { 0x08, 0x14, 0x20, 0x2c };
static const int QUEUE_BUF_OFF[4] = { 0x0c, 0x18, 0x24, 0x30 };

/* CSTGMidiQueue ringCtl field offsets. */
#define RC_MASK   0x08   /* capacity - 1        */
#define RC_WPOS   0x0c   /* write cursor        */
#define RC_RCUR0  0x10   /* reader cursor[0]    */
#define RC_RCOUNT 0x20   /* active reader count (byte) */
#define RC_MAXREADERS 4

/* ------------------------------------------------------------------ */
/*  MIDI IN injection state (unchanged from midi_inject)               */
/* ------------------------------------------------------------------ */

typedef void (*receive_fn_t)(void *, const uint8_t *, uint32_t)
    __attribute__((regparm(3)));

static void *port_obj;
static uint32_t *ports_array;
static struct proc_dir_entry *proc_midi_in;
static struct proc_dir_entry *proc_midi_ring;
static struct proc_dir_entry *proc_midi_ports;

/* Guards the MIDI-IN injection target (port_obj/receive_fn). dead is set by the
 * OA-unload notifier so an in-flight /proc/.midi_in write or a drain can't call
 * into (or read) freed OA memory. */
static DEFINE_SPINLOCK(ring_lock);
static int oa_dead;

/* ------------------------------------------------------------------ */
/*  MIDI OUT tap state                                                 */
/* ------------------------------------------------------------------ */

static unsigned long out_ports;   /* resolved sMidiOutPorts (array of 4 ptrs) */

/* One tapped queue. We claim our own reader slot on each and drain from our own
 * cursor; dumps and performance never overlap (a dump halts all other MIDI), so
 * draining the queues in a fixed order per read reproduces the port's transmitted
 * byte stream with no interleaving. */
struct tapq {
    unsigned long ringctl;   /* CSTGMidiQueue ringCtl                    */
    unsigned long buf;       /* its data buffer                         */
    uint32_t      mask;      /* capacity - 1                            */
    uint32_t      cap;       /* capacity                                */
    uint32_t      cursor;    /* our read cursor (mirrors ringCtl slot)  */
    int           reader_idx;/* our reader slot on this queue           */
};
static struct tapq taps[6];   /* shared q1,q2 + up to 4 per-port q3 */
static int ntaps;             /* number of queues we successfully tapped */

static int kptr_ok(unsigned long p)
{
    return p >= 0x40000000UL && p < 0xfffff000UL && (p & 3) == 0;
}

/* ------------------------------------------------------------------ */
/*  Unified MIDI OUT ring (SPSC: drain producer -> /proc/.midi_ring)   */
/* ------------------------------------------------------------------ */

#define UNI_RING_BITS  16               /* 65536 bytes - burst headroom for multi-MB dumps */
#define UNI_RING_SIZE  (1 << UNI_RING_BITS)
#define UNI_RING_MASK  (UNI_RING_SIZE - 1)

static uint8_t  uni_ring[UNI_RING_SIZE];
static uint32_t uni_wpos = 0;
static uint32_t uni_rpos = 0;
static uint32_t uni_overflow = 0;

/* Drain and consumer both run in the /proc/.midi_ring reader's context (see
 * ring_fops_read), so uni_ring is single-threaded and needs no lock. */

/* ------------------------------------------------------------------ */
/*  Out-queue tap: resolve, claim a reader, drain, release            */
/* ------------------------------------------------------------------ */

/* RegisterMidiOutPort: 0f be 50 04 / 89 04 95 <disp32=&sMidiOutPorts> / c3 */
static unsigned long resolve_out_ports(unsigned long fn)
{
    const uint8_t *p = (const uint8_t *)fn;
    if (!fn) return 0;
    if (p[0] != 0x0f || p[1] != 0xbe || p[2] != 0x50 || p[3] != 0x04 ||
        p[4] != 0x89 || p[5] != 0x04 || p[6] != 0x95)
        return 0;
    return (unsigned long)*(const uint32_t *)(p + 7);
}

/* Claim a spare reader slot on one queue slot of the given out-port object. Mirrors OA's
 * AllocReader (lock xadd on the count byte) without calling into OA .text. On
 * success appends a tapq entry and returns 0; returns -1 if that queue is invalid
 * or already has all 4 reader slots in use (refuse rather than corrupt). */
static int tap_claim_one(unsigned long portp, int qslot)
{
    unsigned long qptr, qbuf;
    volatile uint8_t *rcount;
    uint8_t idx;
    struct tapq *t;

    qptr = *(uint32_t *)(portp + QUEUE_PTR_OFF[qslot]);
    qbuf = *(uint32_t *)(portp + QUEUE_BUF_OFF[qslot]);
    if (!kptr_ok(qptr) || !kptr_ok(qbuf))
        return -1;

    rcount = (volatile uint8_t *)(qptr + RC_RCOUNT);
    if (*rcount >= RC_MAXREADERS)   /* no spare slot - refuse (never corrupt) */
        return -1;
    /* No concurrent AllocReader at runtime (OA builds its out-ports once at boot),
     * so read-then-add is race-free here. */
    idx = __sync_fetch_and_add(rcount, 1);
    if (idx >= RC_MAXREADERS) {      /* raced to full - back out */
        __sync_fetch_and_sub(rcount, 1);
        return -1;
    }

    t = &taps[ntaps++];
    t->ringctl    = qptr;
    t->buf        = qbuf;
    t->mask       = *(uint32_t *)(qptr + RC_MASK);
    t->cap        = t->mask + 1;
    t->reader_idx = idx;
    /* Start from "now" so we don't dump the pre-existing backlog. */
    t->cursor     = *(uint32_t *)(qptr + RC_WPOS);
    *(uint32_t *)(qptr + RC_RCUR0 + idx * 4) = t->cursor;
    return 0;
}

/* Generic capture: tap the SHARED performance queues (q1,q2 - identical across
 * all out-ports, so tapped once) plus EVERY out-port's per-port queue (q3 - where
 * bulk data dumps route, per destination). This yields one destination-agnostic
 * MIDI-out stream: performance appears once (no duplication - verified on hardware,
 * per-port queues don't echo the shared stream), and a dump sent to ANY
 * destination (USB or DIN) is captured. q0 (active-sensing/realtime) is excluded.
 * Returns the number of queues successfully tapped. */
static int tap_claim_reader(void)
{
    unsigned long p0 = 0;
    int i;

    ntaps = 0;
    if (!out_ports)
        return 0;

    /* First activated out-port carries the shared queue pointers. */
    for (i = 0; i < 4; i++) {
        unsigned long v = *(uint32_t *)(out_ports + i * 4);
        if (kptr_ok(v)) { p0 = v; break; }
    }
    if (!p0)
        return 0;

    tap_claim_one(p0, 1);   /* q1 shared (misc)                    */
    tap_claim_one(p0, 2);   /* q2 shared (performance: notes/CC/PC/combi SysEx) */

    /* Per-port bulk-dump queue (q3) from every activated out-port. */
    for (i = 0; i < 4; i++) {
        unsigned long v = *(uint32_t *)(out_ports + i * 4);
        if (kptr_ok(v))
            tap_claim_one(v, 3);
    }
    return ntaps;
}

/* Give every claimed reader slot back on unload. Safe only because OA never grows
 * a queue's reader count at runtime (proven): we are the top reader on each, so
 * decrement + clear our cursor. */
static void tap_release_reader(void)
{
    int i;
    for (i = 0; i < ntaps; i++) {
        struct tapq *t = &taps[i];
        volatile uint8_t *rcount = (volatile uint8_t *)(t->ringctl + RC_RCOUNT);
        if (!t->ringctl || t->reader_idx < 0)
            continue;
        /* Only decrement if the count is still exactly what we left it. */
        if (*rcount == (uint8_t)(t->reader_idx + 1)) {
            *(uint32_t *)(t->ringctl + RC_RCUR0 + t->reader_idx * 4) = 0;
            __sync_fetch_and_sub(rcount, 1);
        }
        t->ringctl = 0;
        t->reader_idx = -1;
    }
    ntaps = 0;
}

/* Drain newly-transmitted bytes from one tapped queue into uni_ring. Best-effort:
 * if we lag we skip our cursor forward rather than hold it back, so we can never
 * throttle OA's real output. */
static void tap_drain_one(struct tapq *t)
{
    uint32_t wpos, avail, space, used, take, i;

    if (!t->ringctl || t->reader_idx < 0 || !t->buf)
        return;

    wpos  = *(volatile uint32_t *)(t->ringctl + RC_WPOS);
    avail = wpos - t->cursor;
    /* If we fell more than a full buffer behind, those bytes were already refused
     * by the writer; resync to the oldest still-valid data. */
    if (avail > t->cap) {
        t->cursor = wpos - t->cap;
        avail = t->cap;
    }

    /* Bound by uni_ring free space FIRST so we never clobber unread bytes. */
    used  = uni_wpos - uni_rpos;
    space = (used < UNI_RING_SIZE) ? (UNI_RING_SIZE - used) : 0;
    take  = (avail < space) ? avail : space;

    for (i = 0; i < take; i++)
        uni_ring[(uni_wpos + i) & UNI_RING_MASK] =
            *(const uint8_t *)(t->buf + ((t->cursor + i) & t->mask));
    uni_wpos  += take;
    t->cursor += take;

    /* Best-effort: drop our copy (not OA's) rather than let our backlog grow. */
    if (wpos != t->cursor) {
        uni_overflow += wpos - t->cursor;
        t->cursor = wpos;
    }
    *(uint32_t *)(t->ringctl + RC_RCUR0 + t->reader_idx * 4) = t->cursor;
}

/* Drain all tapped queues in slot order. A dump halts all other MIDI, so at most
 * one queue has data at a time - fixed-order draining reproduces the transmitted
 * stream with no cross-queue interleaving. */
static void tap_drain(void)
{
    int i;
    for (i = 0; i < ntaps; i++)
        tap_drain_one(&taps[i]);
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ring - MIDI out (drain-on-read)                        */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint32_t avail, take, off, first;
    unsigned long flags;

    /* Drain the tapped queues into uni_ring first, in this reader's context.
     * ring_lock + oa_dead guard against OA teardown mid-drain. */
    spin_lock_irqsave(&ring_lock, flags);
    if (!oa_dead)
        tap_drain();
    spin_unlock_irqrestore(&ring_lock, flags);

    avail = uni_wpos - uni_rpos;
    if (avail == 0)
        return 0;

    /* Serve up to `count` bytes straight from uni_ring (no bounce buffer, so the
     * per-read size isn't capped - the daemon can drain multi-MB dumps fast). The
     * consumer is single-threaded (midi_tcp); copy in at most two chunks to span
     * the ring wrap. */
    take = (avail < count) ? avail : count;
    off  = uni_rpos & UNI_RING_MASK;
    first = UNI_RING_SIZE - off;
    if (first > take) first = take;
    if (copy_to_user(buf, uni_ring + off, first))
        return -EFAULT;
    if (take > first && copy_to_user(buf + first, uni_ring, take - first))
        return -EFAULT;
    uni_rpos += take;
    return (ssize_t)take;
}

/* A write means "reset my read cursor" - midi_tcp does this on a new client so it
 * starts from live output. Drop any uni_ring backlog and resync the tap cursor to
 * live. */
static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    unsigned long flags;
    int i;
    spin_lock_irqsave(&ring_lock, flags);
    uni_rpos = uni_wpos;
    if (!oa_dead)
        for (i = 0; i < ntaps; i++) {
            struct tapq *t = &taps[i];
            t->cursor = *(volatile uint32_t *)(t->ringctl + RC_WPOS);
            *(uint32_t *)(t->ringctl + RC_RCUR0 + t->reader_idx * 4) = t->cursor;
        }
    spin_unlock_irqrestore(&ring_lock, flags);
    return (ssize_t)count;
}

static const struct file_operations ring_fops = {
    .owner = THIS_MODULE,
    .read  = ring_fops_read,
    .write = ring_fops_write,
};

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ports - diagnostic                                     */
/* ------------------------------------------------------------------ */

static int ports_read_proc(char *page, char **start, off_t off,
                           int count, int *eof, void *data)
{
    int len = 0, i;
    len += sprintf(page + len, "out_ports=0x%lx ntaps=%d overflow=%u\n",
                   out_ports, ntaps, uni_overflow);
    for (i = 0; i < ntaps; i++) {
        struct tapq *t = &taps[i];
        uint8_t rc = *(volatile uint8_t *)(t->ringctl + RC_RCOUNT);
        uint32_t wpos = *(volatile uint32_t *)(t->ringctl + RC_WPOS);
        len += sprintf(page + len,
                       "tap[%d] ringctl=0x%lx buf=0x%lx cap=%u readerIdx=%d wpos=%u cursor=%u readers=%u\n",
                       i, t->ringctl, t->buf, t->cap, t->reader_idx, wpos, t->cursor, rc);
    }
    if (ntaps == 0)
        len += sprintf(page + len, "(no tap active - port unresolved or all queues full)\n");
    *eof = 1;
    return len;
}

/* ------------------------------------------------------------------ */
/*  MIDI IN: port discovery + injection (unchanged from midi_inject)   */
/* ------------------------------------------------------------------ */

static void *find_port_object(void)
{
    uint8_t *fn_bytes;
    int i;

    if (!register_fn) return NULL;

    fn_bytes = (uint8_t *)register_fn;
    if (fn_bytes[0] != 0x0f || fn_bytes[1] != 0xbe ||
        fn_bytes[4] != 0x89 || fn_bytes[5] != 0x04 || fn_bytes[6] != 0x95) {
        printk(KERN_ERR "midi_bridge: RegisterMidiInPort pattern mismatch\n");
        return NULL;
    }

    ports_array = (uint32_t *)*(uint32_t *)(fn_bytes + 7);
    printk(KERN_INFO "midi_bridge: sMidiInPorts at %p\n", ports_array);

    for (i = 0; i < 4; i++) {
        uint32_t addr = ports_array[i];
        if (addr > 0x40000000) {
            uint8_t flags = ((uint8_t *)(unsigned long)addr)[0x26];
            if (flags & 0x02)
                return (void *)(unsigned long)addr;
        }
    }
    return NULL;
}

#define MIDI_INJECT_MAX 4096

static int midi_write(struct file *f, const char __user *buf,
                      unsigned long count, void *data)
{
    receive_fn_t fn;
    void *obj;
    uint8_t *kbuf;
    int len = count > MIDI_INJECT_MAX ? MIDI_INJECT_MAX : count;
    unsigned long flags;

    spin_lock_irqsave(&ring_lock, flags);
    if (oa_dead || !port_obj || !receive_fn) {
        spin_unlock_irqrestore(&ring_lock, flags);
        return -ENODEV;
    }
    fn  = (receive_fn_t)receive_fn;
    obj = port_obj;
    spin_unlock_irqrestore(&ring_lock, flags);

    if (len <= 0)
        return count;
    /* Per-call buffer: /proc/.midi_in is world-writable, concurrent writers must
     * not share one buffer (would tear a multi-byte MIDI/SysEx frame). */
    kbuf = kmalloc(len, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, buf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }
    fn(obj, kbuf, len);
    kfree(kbuf);
    return count;
}

/* ------------------------------------------------------------------ */
/*  OA-unload notifier                                                 */
/* ------------------------------------------------------------------ */

static int midi_module_notify(struct notifier_block *nb,
                              unsigned long action, void *data)
{
    struct module *mod = data;
    if (action == MODULE_STATE_GOING &&
        (strcmp(mod->name, "OA") == 0 || strcmp(mod->name, "loadmod") == 0)) {
        unsigned long flags;
        /* OA is being torn down ("Preparing to Install"). Stop touching OA memory:
         * disable injection and the tap. No .text to restore, no trampoline to
         * leak - the hook-free design has nothing to undo here beyond releasing
         * our reader slot while OA memory is still valid. */
        spin_lock_irqsave(&ring_lock, flags);
        oa_dead   = 1;
        port_obj  = NULL;
        tap_release_reader();
        spin_unlock_irqrestore(&ring_lock, flags);
        printk(KERN_INFO "midi_bridge: %s unloading, MIDI disabled\n", mod->name);
    }
    return NOTIFY_OK;
}

static struct notifier_block midi_nb = {
    .notifier_call = midi_module_notify,
};

/* ------------------------------------------------------------------ */
/*  Deferred setup worker (RTAI: create_proc_entry etc. off init_module)*/
/* ------------------------------------------------------------------ */

static struct work_struct midi_work;

static void midi_setup(struct work_struct *work)
{
    int have_out = 0;

    if (receive_fn && register_fn) {
        port_obj = find_port_object();
        if (!port_obj)
            printk(KERN_WARNING "midi_bridge: MIDI IN port discovery failed "
                   "(pattern mismatch?) - IN injection unavailable\n");
    } else {
        printk(KERN_WARNING "midi_bridge: receive_fn/register_fn not set - MIDI IN unavailable\n");
    }

    if (regoutport) {
        out_ports = resolve_out_ports(regoutport);
        if (out_ports && tap_claim_reader() > 0) {
            int i;
            have_out = 1;
            printk(KERN_INFO "midi_bridge: generic out tap, %d queue(s):\n", ntaps);
            for (i = 0; i < ntaps; i++)
                printk(KERN_INFO "midi_bridge:   tap[%d] ringctl=0x%lx cap=%u readerIdx=%d\n",
                       i, taps[i].ringctl, taps[i].cap, taps[i].reader_idx);
        } else {
            printk(KERN_WARNING "midi_bridge: out-queue tap unavailable "
                   "(unresolved or no free reader slot)\n");
        }
    } else {
        printk(KERN_WARNING "midi_bridge: regoutport not set - MIDI OUT capture unavailable\n");
    }

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (proc_midi_in)
        proc_midi_in->write_proc = midi_write;
    else
        printk(KERN_ERR "midi_bridge: create_proc_entry(.midi_in) failed\n");

    if (have_out) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;

        proc_midi_ports = create_proc_entry(".midi_ports", 0444, NULL);
        if (proc_midi_ports)
            proc_midi_ports->read_proc = ports_read_proc;
    }

    /* Arm the OA-unload notifier only after tap state is established. */
    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_bridge: ready - in=%s out=%s\n",
           port_obj ? "ok" : "none",
           have_out ? "ok" : "none");
}

static int __init midi_bridge_init(void)
{
    printk(KERN_INFO "midi_bridge: queue-tap build\n");
    printk(KERN_INFO "midi_bridge: receive=0x%lx register=0x%lx regoutport=0x%lx\n",
           receive_fn, register_fn, regoutport);
    INIT_WORK(&midi_work, midi_setup);
    schedule_work(&midi_work);
    return 0;
}

static void __exit midi_bridge_exit(void)
{
    unsigned long flags;
    flush_scheduled_work();
    unregister_module_notifier(&midi_nb);

    /* Release our reader slot while OA memory is still valid (unless OA already
     * went away, in which case the notifier already released it). */
    spin_lock_irqsave(&ring_lock, flags);
    if (!oa_dead)
        tap_release_reader();
    oa_dead  = 1;
    port_obj = NULL;
    spin_unlock_irqrestore(&ring_lock, flags);

    if (proc_midi_ports)
        remove_proc_entry(".midi_ports", NULL);
    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_bridge: unloaded\n");
}

module_init(midi_bridge_init);
module_exit(midi_bridge_exit);
