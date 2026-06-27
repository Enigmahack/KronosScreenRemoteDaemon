/*
 * midi_inject.ko — MIDI injection + output capture for Korg Kronos
 *
 * /proc/.midi_in   — write raw MIDI bytes to inject into OA.ko MIDI IN
 * /proc/.midi_ring — read MIDI output: unified ring fed by ReadNextMessage hook
 *                    and Block 5 SysEx drain
 *
 * Module params:
 *   receive_fn=0x...        MidiInPortGeneric7Receive (required for MIDI IN injection)
 *   register_fn=0x...       RegisterMidiInPort (required for port object discovery)
 *   midi_dispatch_fn=0x...  CSTGMidiOutPort::ReadNextMessage VA (all MIDI out)
 *                           Mangled: _ZN15CSTGMidiOutPort15ReadNextMessageEPhj
 *                           Kronos 1 (D510, OA@0x59d17000): 0x59E054A0
 *   can_send_fn=0x...       KorgUsbMidiOutputCanSend — hooked to always return 1
 *                           so the USB output thread calls ReadNextMessage even
 *                           without a USB host connected. Linux-context safe.
 *                           Kronos 1: 0x58f70390
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <asm/cacheflush.h>

MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */
/*  Module parameters                                                  */
/* ------------------------------------------------------------------ */

static unsigned long receive_fn = 0;
module_param(receive_fn, ulong, 0444);

static unsigned long register_fn = 0;
module_param(register_fn, ulong, 0444);

static unsigned long midi_dispatch_fn = 0;
module_param(midi_dispatch_fn, ulong, 0444);

static unsigned long can_send_fn = 0;
module_param(can_send_fn, ulong, 0444);

/* ------------------------------------------------------------------ */
/*  MIDI IN injection state                                            */
/* ------------------------------------------------------------------ */

typedef void (*receive_fn_t)(void *, const uint8_t *, uint32_t)
    __attribute__((regparm(3)));

static void *port_obj;
static uint32_t *ports_array;
static struct proc_dir_entry *proc_midi_in;
static struct proc_dir_entry *proc_midi_ring;
static uint8_t midi_buf[4096];

/* ------------------------------------------------------------------ */
/*  Block 5 ring state (SysEx responses via Korg internal path)       */
/* ------------------------------------------------------------------ */

static uint32_t *hw_queue;
static uint8_t  *hw_data;
static uint32_t  hw_mask;
static uint32_t  ring_cursor;

static DEFINE_SPINLOCK(ring_lock);
static int ring_dead;

static void ring_disable(void)
{
    spin_lock(&ring_lock);
    ring_dead = 1;
    hw_data   = NULL;
    hw_queue  = NULL;
    port_obj  = NULL;
    spin_unlock(&ring_lock);
}

/* ------------------------------------------------------------------ */
/*  Unified MIDI OUT ring (dispatcher hook + Block 5 drain)           */
/* ------------------------------------------------------------------ */

#define UNI_RING_BITS  14               /* 16384 bytes */
#define UNI_RING_SIZE  (1 << UNI_RING_BITS)
#define UNI_RING_MASK  (UNI_RING_SIZE - 1)

static uint8_t  uni_ring[UNI_RING_SIZE];
static uint32_t uni_wpos = 0;
static uint32_t uni_rpos = 0;
static DEFINE_SPINLOCK(uni_lock);

static void uni_ring_push_buf(const uint8_t *buf, uint32_t len)
{
    unsigned long flags;
    uint32_t i, used, space;

    /* Use trylock so this path never blocks.  Called potentially from the
     * kOAMidiOutput thread (Linux) or an RTAI RT context via the
     * ReadNextMessage hook.  Blocking here causes priority inversion against
     * ring_fops_read (held by midi_tcp) which stalls the RT domain and makes
     * joystick / CC inputs lag.  Drop data rather than interfere. */
    if (!spin_trylock_irqsave(&uni_lock, flags))
        return;

    used  = uni_wpos - uni_rpos;
    space = (used < UNI_RING_SIZE) ? (UNI_RING_SIZE - used) : 0;
    if (len > space)
        len = space;   /* ring full: drop new data, never overwrite unread */

    for (i = 0; i < len; i++) {
        uni_ring[uni_wpos & UNI_RING_MASK] = buf[i];
        uni_wpos++;
    }
    spin_unlock_irqrestore(&uni_lock, flags);
}

/* Drain any new Block 5 bytes into uni_ring.  Called from ring_fops_read. */
static void drain_blk5_to_uni(void)
{
    uint8_t  tmp[128];
    uint8_t  *data;
    uint32_t *queue;
    uint32_t  mask, wpos, avail, take, first;

    spin_lock(&ring_lock);
    if (ring_dead || !hw_data || !hw_queue) { spin_unlock(&ring_lock); return; }
    data  = hw_data;
    queue = hw_queue;
    mask  = hw_mask;
    spin_unlock(&ring_lock);

    if (probe_kernel_read(&wpos, &queue[3], sizeof(wpos))) {
        ring_disable();
        return;
    }
    avail = wpos - ring_cursor;
    if (!avail) return;
    if (avail > mask + 1) avail = mask + 1;

    while (avail > 0) {
        take  = avail < (uint32_t)sizeof(tmp) ? avail : (uint32_t)sizeof(tmp);
        first = (mask + 1) - (ring_cursor & mask);
        if (first > take) first = take;

        if (probe_kernel_read(tmp, &data[ring_cursor & mask], first)) {
            ring_disable(); return;
        }
        if (first < take) {
            if (probe_kernel_read(tmp + first, &data[0], take - first)) {
                ring_disable(); return;
            }
        }
        uni_ring_push_buf(tmp, take);
        ring_cursor += take;
        avail       -= take;
    }
}

/* ------------------------------------------------------------------ */
/*  Inline hook infrastructure                                         */
/* ------------------------------------------------------------------ */

static uint8_t *tramp_dispatch  = NULL;  static uint8_t orig_dispatch[6];
static uint8_t *tramp_can_send  = NULL;  static uint8_t orig_can_send[6];

/* Post-call hook for CSTGMidiOutPort::ReadNextMessage — regparm(3): self→EAX, buf→EDX, maxlen→ECX.
 * Calls original first (fills buf), then captures the returned bytes into the unified ring. */
static int __attribute__((regparm(3)))
hook_read_next_msg(void *self, uint8_t *buf, uint32_t maxlen)
{
    typedef int (*orig_t)(void *, uint8_t *, uint32_t)
        __attribute__((regparm(3)));
    int n = ((orig_t)tramp_dispatch)(self, buf, maxlen);
    if (n > 0 && (uint32_t)n <= maxlen)
        uni_ring_push_buf(buf, (uint32_t)n);
    return n;
}

/* Hook KorgUsbMidiOutputCanSend to always return 1 so the USB MIDI output
 * thread unconditionally calls ReadNextMessage even without a USB host.
 * Called from Linux context (kOAMidiOutput thread) — vmalloc-safe. */
static int hook_can_send(void) { return 1; }

/* Temporarily disable CR0.WP so kernel .text is writable. */
static unsigned long saved_cr0;
static void wp_disable(void)
{
    asm volatile("mov %%cr0, %0" : "=r"(saved_cr0));
    asm volatile("mov %0, %%cr0" :: "r"(saved_cr0 & ~0x10000UL));
}
static void wp_enable(void)
{
    asm volatile("mov %0, %%cr0" :: "r"(saved_cr0));
}

/* Write a 5-byte relative JMP at dst jumping to tgt. */
static void write_jmp(uint8_t *dst, uint8_t *tgt)
{
    dst[0] = 0xE9;
    *(int32_t *)(dst + 1) = (int32_t)(tgt - (dst + 5));
}

/* Install inline hook:
 *   - allocates 16-byte executable trampoline
 *   - saves save_len original bytes into orig[] (must be ≥ 5, padded to instruction boundary)
 *   - builds trampoline: orig[save_len] + JMP back to fn+save_len
 *   - patches fn[0..4] with JMP to hook_fn (always a 5-byte E9 rel32)
 *
 * save_len must be ≥ 5.  Use 6 for standard i386 cdecl/regparm prologues where
 * the third instruction (and/sub esp) spans bytes 3–5 of the function.
 *
 * Returns 0 on success, negative on failure (hook NOT installed). */
static int install_hook(unsigned long fn_addr, void *hook_fn,
                        uint8_t **tramp_ptr, uint8_t *orig, int save_len)
{
    uint8_t *fn = (uint8_t *)fn_addr;
    uint8_t *tramp;
    unsigned long flags;

    if (!fn_addr || save_len < 5) return -EINVAL;

    /* Allocate trampoline from the direct-mapped region (get_free_page) so it is
     * always present in kernel page tables — no lazy vmalloc fault.  This is
     * required for hooks called from RTAI real-time tasks, whose trap handler
     * does not recover vmalloc #PF.  set_memory_x makes the page executable
     * (needed on PAE kernels with NX support). */
    tramp = (uint8_t *)__get_free_page(GFP_KERNEL);
    if (!tramp) {
        printk(KERN_ERR "midi_inject: get_free_page trampoline failed\n");
        return -ENOMEM;
    }
    set_memory_x((unsigned long)tramp, 1);
    *tramp_ptr = tramp;

    /* Copy save_len bytes (covering all instructions that overlap the 5-byte patch) */
    memcpy(orig, fn, save_len);
    memcpy(tramp, orig, save_len);
    write_jmp(tramp + save_len, fn + save_len);

    /* Patch target: write 5-byte JMP to hook.  Bytes 5..save_len-1 of fn are
     * left unchanged — they are part of instructions already in the trampoline. */
    local_irq_save(flags);
    wp_disable();
    write_jmp(fn, (uint8_t *)hook_fn);
    wp_enable();
    local_irq_restore(flags);

    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");

    printk(KERN_INFO "midi_inject: hooked 0x%lx → %p tramp=%p\n",
           fn_addr, hook_fn, tramp);
    return 0;
}

/* 1 while our 5-byte JMPs are live in the target .text.  Guards against
 * restoring OA's bytes twice (once from the OA-unload notifier, once from
 * module_exit) and against writing into OA .text after OA is already gone. */
static int hooks_patched = 0;

/* Restore the 5 patched bytes in the target .text.  LOCAL-ONLY: IRQ-disabled
 * byte copy + CR0.WP toggle.  No IPI, no allocation, no sleep — therefore SAFE
 * to call from the module notifier, which runs under module_mutex during another
 * module's teardown. */
static void restore_hook_bytes(unsigned long fn_addr, uint8_t *orig)
{
    unsigned long flags;
    if (!fn_addr) return;
    local_irq_save(flags);
    wp_disable();
    memcpy((uint8_t *)fn_addr, orig, 5);  /* restore only the 5 bytes we patched */
    wp_enable();
    local_irq_restore(flags);
    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");
    printk(KERN_INFO "midi_inject: unhooked 0x%lx\n", fn_addr);
}

/* Free a trampoline page.  set_memory_nx() issues a cross-CPU TLB-flush IPI and
 * waits for every CPU to ACK — on the RTAI kernel an RT-controlled core may never
 * service it, so on_each_cpu() spins forever and the whole system freezes.  This
 * MUST run only in process context (module_exit), NEVER from the module notifier. */
static void free_trampoline(uint8_t **tramp_ptr)
{
    if (*tramp_ptr) {
        set_memory_nx((unsigned long)*tramp_ptr, 1);
        free_page((unsigned long)*tramp_ptr);
        *tramp_ptr = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Module notifier — disable on OA/loadmod unload                    */
/* ------------------------------------------------------------------ */

static int midi_module_notify(struct notifier_block *nb,
                              unsigned long action, void *data)
{
    struct module *mod = data;
    if (action == MODULE_STATE_GOING &&
        (strcmp(mod->name, "OA") == 0 ||
         strcmp(mod->name, "loadmod") == 0)) {
        /* OA is being torn down.  This is exactly what the Kronos does at
         * "Preparing to Install" before an OS update.  Do the MINIMUM that is
         * safe under module_mutex on the RTAI kernel: disable the ring and
         * restore OA's patched bytes via the local-only path.  Do NOT free
         * trampolines here — free_trampoline()/set_memory_nx() issues an IPI
         * that can hang the RT kernel and freeze the whole system (this was the
         * cause of installs/cleaners freezing at "Preparing to Install").  The
         * trampoline pages are reclaimed later at rmmod, in process context. */
        ring_disable();
        /* Serialized against module_exit by module_mutex; hook fns never read this. */
        if (hooks_patched) {
            hooks_patched = 0;
            restore_hook_bytes(can_send_fn,      orig_can_send);
            restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        }
        printk(KERN_INFO "midi_inject: %s unloading, hooks removed (free deferred)\n",
               mod->name);
    }
    return NOTIFY_OK;
}

static struct notifier_block midi_nb = {
    .notifier_call = midi_module_notify,
};

/* ------------------------------------------------------------------ */
/*  Port discovery (MIDI IN side)                                      */
/* ------------------------------------------------------------------ */

static void *find_port_object(void)
{
    uint8_t *fn_bytes;
    int i;

    if (!register_fn) return NULL;

    fn_bytes = (uint8_t *)register_fn;
    if (fn_bytes[0] != 0x0f || fn_bytes[1] != 0xbe ||
        fn_bytes[4] != 0x89 || fn_bytes[5] != 0x04 || fn_bytes[6] != 0x95) {
        printk(KERN_ERR "midi_inject: RegisterMidiInPort pattern mismatch\n");
        return NULL;
    }

    ports_array = (uint32_t *)*(uint32_t *)(fn_bytes + 7);
    printk(KERN_INFO "midi_inject: sMidiInPorts at %p\n", ports_array);

    for (i = 0; i < 4; i++) {
        uint32_t addr = ports_array[i];
        if (addr > 0x40000000) {
            uint8_t *p = (uint8_t *)(unsigned long)addr;
            printk(KERN_INFO "midi_inject: ports[%d]=%p idx=%d flags=0x%02x %s\n",
                   i, p, p[0x25], p[0x26], (p[0x26] & 0x02) ? "ACTIVE" : "");
        }
    }

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

/* ------------------------------------------------------------------ */
/*  Block 5 ring setup (SysEx responses)                              */
/* ------------------------------------------------------------------ */

static int setup_ring(void)
{
    uint32_t port0_q100, container_kva, blk4_data_kva;
    uint32_t *q;

    if (ports_array[0] < 0x40000000) return -ENODEV;

    port0_q100 = *(uint32_t *)((uint8_t *)(unsigned long)ports_array[0] + 0x100);
    if (port0_q100 < 0x80000000) return -ENODEV;

    container_kva = port0_q100;
    q = (uint32_t *)(unsigned long)(container_kva + 0x190);

    printk(KERN_INFO "midi_inject: container=0x%08x queue@%p blk=%u mask=0x%x wpos=%u\n",
           container_kva, q, q[0], q[2], q[3]);

    if (q[0] != 5 || q[2] != 0x7F) {
        printk(KERN_ERR "midi_inject: unexpected Block 5 queue (blk=%u mask=0x%x)\n",
               q[0], q[2]);
        return -EINVAL;
    }

    hw_queue = q;
    hw_mask  = q[2];

    blk4_data_kva = *(uint32_t *)((uint8_t *)(unsigned long)ports_array[0] + 0x104);
    hw_data = (uint8_t *)(unsigned long)(blk4_data_kva - 0x80);

    ring_cursor = q[3];

    printk(KERN_INFO "midi_inject: blk5 data=%p mask=0x%x\n", hw_data, hw_mask);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ring — unified MIDI out (dispatcher hook + Block 5)   */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint8_t tmp[512];
    size_t len = 0;
    unsigned long flags;
    uint32_t wpos, avail;
    size_t take, i;

    /* Drain Block 5 (Korg internal SysEx) into unified ring */
    drain_blk5_to_uni();

    /* Read from unified ring */
    spin_lock_irqsave(&uni_lock, flags);
    wpos  = uni_wpos;
    avail = wpos - uni_rpos;
    if (avail > 0) {
        take = avail;
        if (take > sizeof(tmp)) take = sizeof(tmp);
        if (take > count)       take = count;
        for (i = 0; i < take; i++)
            tmp[i] = uni_ring[(uni_rpos + i) & UNI_RING_MASK];
        uni_rpos += take;
        len = take;
    }
    spin_unlock_irqrestore(&uni_lock, flags);

    if (len == 0) return 0;
    if (copy_to_user(buf, tmp, len)) return -EFAULT;
    return (ssize_t)len;
}

static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    unsigned long flags;
    uint32_t wpos;

    /* Reset Block 5 cursor to current write position */
    spin_lock(&ring_lock);
    if (!ring_dead && hw_queue &&
        !probe_kernel_read(&wpos, &hw_queue[3], sizeof(wpos)))
        ring_cursor = wpos;
    spin_unlock(&ring_lock);

    /* Reset unified ring read cursor */
    spin_lock_irqsave(&uni_lock, flags);
    uni_rpos = uni_wpos;
    spin_unlock_irqrestore(&uni_lock, flags);

    return (ssize_t)count;
}

static const struct file_operations ring_fops = {
    .owner = THIS_MODULE,
    .read  = ring_fops_read,
    .write = ring_fops_write,
};

/* ------------------------------------------------------------------ */
/*  /proc/.midi_in — MIDI injection                                    */
/* ------------------------------------------------------------------ */

static int midi_write(struct file *f, const char __user *buf,
                      unsigned long count, void *data)
{
    receive_fn_t fn;
    void *obj;
    int len = count > sizeof(midi_buf) ? sizeof(midi_buf) : count;

    spin_lock(&ring_lock);
    if (ring_dead || !port_obj || !receive_fn) {
        spin_unlock(&ring_lock);
        return -ENODEV;
    }
    fn  = (receive_fn_t)receive_fn;
    obj = port_obj;
    spin_unlock(&ring_lock);

    if (copy_from_user(midi_buf, buf, len)) return -EFAULT;

    fn(obj, midi_buf, len);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                 */
/* ------------------------------------------------------------------ */

static int __init midi_inject_init(void)
{
    int ret;
    int have_ring = 0;

    printk(KERN_INFO "midi_inject: receive=0x%lx register=0x%lx dispatch=0x%lx can_send=0x%lx\n",
           receive_fn, register_fn, midi_dispatch_fn, can_send_fn);

    /* MIDI IN injection — port discovery uses a bytecode pattern in RegisterMidiInPort
     * that is specific to the compiled OA.ko.  If it fails (different OS or model),
     * degrade gracefully: output capture hooks still load. */
    if (receive_fn && register_fn) {
        port_obj = find_port_object();
        if (!port_obj) {
            printk(KERN_WARNING "midi_inject: port discovery failed (pattern mismatch?) "
                   "— MIDI IN injection unavailable, output capture unaffected\n");
        } else {
            ret = setup_ring();
            if (ret < 0)
                printk(KERN_WARNING "midi_inject: Block 5 ring setup failed (%d)\n", ret);
            else
                have_ring = 1;
        }
    } else {
        printk(KERN_WARNING "midi_inject: receive_fn/register_fn not set — MIDI IN unavailable\n");
    }

    /* CanSend hook: forces USB MIDI output thread to always call ReadNextMessage,
     * even without a USB host.  Falls back gracefully — USB submission fails silently
     * when no host is present, which is normal operation for the Kronos.
     * Symbol is in KorgUsbAudioDriver.ko; if not found on this model the hook
     * is simply skipped and ReadNextMessage only fires when USB host is connected. */
    if (can_send_fn)
        install_hook(can_send_fn, hook_can_send, &tramp_can_send, orig_can_send, 6);

    if (midi_dispatch_fn) {
        /* save_len=6: ReadNextMessage prologue crosses byte 5 (and esp,0xfffffff0 = 83 E4 F0) */
        if (install_hook(midi_dispatch_fn, hook_read_next_msg,
                         &tramp_dispatch, orig_dispatch, 6) == 0)
            have_ring = 1;
        else
            printk(KERN_WARNING "midi_inject: ReadNextMessage hook failed\n");
    }

    if (!have_ring && !tramp_can_send) {
        printk(KERN_ERR "midi_inject: no output path active — aborting\n");
        return -ENODEV;
    }

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (!proc_midi_in) return -ENOMEM;
    proc_midi_in->write_proc = midi_write;

    if (have_ring) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;
    }

    if (tramp_can_send || tramp_dispatch)
        hooks_patched = 1;

    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_inject: ready — port=%p blk5=%s can_send=%s dispatch=%s\n",
           port_obj,
           hw_data        ? "ok" : "none",
           tramp_can_send ? "ok" : "none",
           tramp_dispatch ? "ok" : "none");
    return 0;
}

static void __exit midi_inject_exit(void)
{
    unregister_module_notifier(&midi_nb);

    /* If the OA-unload notifier already ran, OA is gone and hooks_patched is 0 —
     * do NOT write into the freed OA .text.  Otherwise (normal rmmod with OA
     * still loaded) restore the bytes and let in-flight hook calls drain. */
    if (hooks_patched) {
        hooks_patched = 0;
        restore_hook_bytes(can_send_fn,      orig_can_send);
        restore_hook_bytes(midi_dispatch_fn, orig_dispatch);
        synchronize_rcu();  /* wait for all CPUs to leave the hook paths */
    }

    ring_disable();

    /* Free trampoline pages here, in process context, where set_memory_nx()'s
     * IPI is safe (this is why the notifier deferred the free to us). */
    free_trampoline(&tramp_can_send);
    free_trampoline(&tramp_dispatch);

    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_inject: unloaded\n");
}

module_init(midi_inject_init);
module_exit(midi_inject_exit);
