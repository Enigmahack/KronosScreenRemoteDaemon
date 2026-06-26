/*
 * midi_inject.ko — MIDI injection + output capture for Korg Kronos
 *
 * /proc/.midi_in   — write raw MIDI bytes to inject into OA.ko MIDI IN
 * /proc/.midi_ring — read MIDI output: merges DIN MIDI OUT hook + Block 5 SysEx
 *
 * Module params:
 *   receive_fn=0x...   MidiInPortGeneric7Receive (required for MIDI IN injection)
 *   register_fn=0x...  RegisterMidiInPort (required for port object discovery)
 *   midi_out_fn=0x...  CSTGMidiOutPortSerial::SendSingleByte(unsigned char)
 *   midi_rt_fn=0x...   CSTGMidiOutPortSerial::SendRealTime(unsigned char)
 *
 * When midi_out_fn / midi_rt_fn are provided, an inline hook is installed on
 * each function.  Every byte the Kronos sends to DIN MIDI OUT is copied into a
 * software ring buffer and returned by /proc/.midi_ring reads.  Block 5 (Korg
 * internal SysEx path) is also drained and appended so SysEx responses from
 * the SYSEX control command continue to work.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>

MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */
/*  Module parameters                                                  */
/* ------------------------------------------------------------------ */

static unsigned long receive_fn = 0;
module_param(receive_fn, ulong, 0444);

static unsigned long register_fn = 0;
module_param(register_fn, ulong, 0444);

static unsigned long midi_out_fn = 0;
module_param(midi_out_fn, ulong, 0444);

static unsigned long midi_rt_fn = 0;
module_param(midi_rt_fn, ulong, 0444);

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
/*  DIN MIDI OUT capture ring                                          */
/* ------------------------------------------------------------------ */

#define OUT_RING_BITS  13               /* 8192 bytes */
#define OUT_RING_SIZE  (1 << OUT_RING_BITS)
#define OUT_RING_MASK  (OUT_RING_SIZE - 1)

static uint8_t  out_ring[OUT_RING_SIZE];
static uint32_t out_ring_wpos = 0;
static uint32_t out_ring_rpos = 0;
static DEFINE_SPINLOCK(out_ring_lock);

static void out_ring_push(uint8_t byte)
{
    unsigned long flags;
    spin_lock_irqsave(&out_ring_lock, flags);
    out_ring[out_ring_wpos & OUT_RING_MASK] = byte;
    out_ring_wpos++;
    spin_unlock_irqrestore(&out_ring_lock, flags);
}

/* ------------------------------------------------------------------ */
/*  Inline hook infrastructure                                         */
/* ------------------------------------------------------------------ */

/* Executable trampoline memory: original 5 bytes + JMP rel32 back */
static uint8_t *tramp_out = NULL;
static uint8_t *tramp_rt  = NULL;

static uint8_t orig_out[5];
static uint8_t orig_rt[5];

/* Hook handlers — calling convention regparm(3): this→EAX, byte→EDX.
 * Captures the byte then calls the original via the trampoline. */
static void __attribute__((regparm(3)))
hook_send_single_byte(void *self, uint8_t byte)
{
    typedef void (*orig_t)(void *, uint8_t) __attribute__((regparm(3)));
    out_ring_push(byte);
    ((orig_t)tramp_out)(self, byte);
}

static void __attribute__((regparm(3)))
hook_send_real_time(void *self, uint8_t byte)
{
    typedef void (*orig_t)(void *, uint8_t) __attribute__((regparm(3)));
    out_ring_push(byte);
    ((orig_t)tramp_rt)(self, byte);
}

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
 *   - allocates executable trampoline memory
 *   - saves original 5 bytes into orig[]
 *   - builds trampoline: orig[5] + JMP back to fn+5
 *   - patches fn[0..4] with JMP to hook_fn
 * Returns 0 on success, negative on failure (hook NOT installed). */
static int install_hook(unsigned long fn_addr, void *hook_fn,
                        uint8_t **tramp_ptr, uint8_t *orig)
{
    uint8_t *fn = (uint8_t *)fn_addr;
    uint8_t *tramp;
    unsigned long flags;

    if (!fn_addr) return -EINVAL;

    /* Allocate executable memory for the trampoline */
#ifdef PAGE_KERNEL_EXEC
    tramp = __vmalloc(16, GFP_KERNEL, PAGE_KERNEL_EXEC);
#else
    tramp = vmalloc(16);
#endif
    if (!tramp) {
        printk(KERN_ERR "midi_inject: vmalloc trampoline failed\n");
        return -ENOMEM;
    }
    *tramp_ptr = tramp;

    /* Build trampoline: [5 original bytes][JMP fn+5] */
    memcpy(orig, fn, 5);
    memcpy(tramp, orig, 5);
    write_jmp(tramp + 5, fn + 5);

    /* Patch target: write JMP to hook under disabled write-protect */
    local_irq_save(flags);
    wp_disable();
    write_jmp(fn, (uint8_t *)hook_fn);
    wp_enable();
    local_irq_restore(flags);

    /* Flush instruction cache on all CPUs */
    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");

    printk(KERN_INFO "midi_inject: hooked 0x%lx → %p tramp=%p\n",
           fn_addr, hook_fn, tramp);
    return 0;
}

static void remove_hook(unsigned long fn_addr, uint8_t *orig, uint8_t **tramp_ptr)
{
    if (!fn_addr) return;
    {
        unsigned long flags;
        local_irq_save(flags);
        wp_disable();
        memcpy((uint8_t *)fn_addr, orig, 5);
        wp_enable();
        local_irq_restore(flags);
    }
    /* x86 has coherent instruction cache — no explicit flush needed after
     * patching .text.  Compiler barrier prevents reordering around the patch. */
    asm volatile("" ::: "memory");
    if (*tramp_ptr) {
        vfree(*tramp_ptr);
        *tramp_ptr = NULL;
    }
    printk(KERN_INFO "midi_inject: unhooked 0x%lx\n", fn_addr);
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
        ring_disable();
        /* Unhook before the function addresses become invalid */
        remove_hook(midi_out_fn, orig_out, &tramp_out);
        remove_hook(midi_rt_fn,  orig_rt,  &tramp_rt);
        printk(KERN_INFO "midi_inject: %s unloading, disabled\n", mod->name);
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
/*  /proc/.midi_ring — merged DIN out hook ring + Block 5             */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint8_t tmp[512];
    size_t len = 0;

    /* Source 1: DIN MIDI OUT hook ring (keyboard notes, CC, SysEx via DIN) */
    {
        unsigned long flags;
        uint32_t wpos, avail;
        spin_lock_irqsave(&out_ring_lock, flags);
        wpos  = out_ring_wpos;
        avail = wpos - out_ring_rpos;
        if (avail > 0) {
            size_t i;
            size_t take = avail;
            if (take > sizeof(tmp)) take = sizeof(tmp);
            if (take > count)       take = count;
            for (i = 0; i < take; i++)
                tmp[i] = out_ring[(out_ring_rpos + i) & OUT_RING_MASK];
            out_ring_rpos += take;
            len = take;
        }
        spin_unlock_irqrestore(&out_ring_lock, flags);
    }

    /* Source 2: Block 5 (Korg internal SysEx path, e.g. SYSEX command responses) */
    if (len < sizeof(tmp)) {
        uint8_t  *data;
        uint32_t *queue;
        uint32_t  mask;
        uint32_t  wpos, avail;
        size_t    take, first;

        spin_lock(&ring_lock);
        if (ring_dead || !hw_data || !hw_queue) {
            spin_unlock(&ring_lock);
            goto out;
        }
        data  = hw_data;
        queue = hw_queue;
        mask  = hw_mask;
        spin_unlock(&ring_lock);

        if (probe_kernel_read(&wpos, &queue[3], sizeof(wpos))) {
            ring_disable();
            printk(KERN_WARNING "midi_inject: ring memory gone\n");
            goto out;
        }

        avail = wpos - ring_cursor;
        if (avail == 0) goto out;
        if (avail > (mask + 1)) avail = mask + 1;

        take = avail;
        if (take > sizeof(tmp) - len) take = sizeof(tmp) - len;
        if (take > count - len)       take = count - len;

        first = (mask + 1) - (ring_cursor & mask);
        if (first > take) first = take;

        if (probe_kernel_read(tmp + len, &data[ring_cursor & mask], first)) {
            ring_disable();
            goto out;
        }
        if (first < take) {
            if (probe_kernel_read(tmp + len + first, &data[0], take - first)) {
                ring_disable();
                goto out;
            }
        }
        ring_cursor += take;
        len += take;
    }

out:
    if (len == 0) return 0;
    if (copy_to_user(buf, tmp, len)) return -EFAULT;
    return (ssize_t)len;
}

static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    /* Reset both ring read cursors to current write positions */
    {
        unsigned long flags;
        spin_lock_irqsave(&out_ring_lock, flags);
        out_ring_rpos = out_ring_wpos;
        spin_unlock_irqrestore(&out_ring_lock, flags);
    }
    {
        uint32_t wpos;
        spin_lock(&ring_lock);
        if (!ring_dead && hw_queue &&
            !probe_kernel_read(&wpos, &hw_queue[3], sizeof(wpos)))
            ring_cursor = wpos;
        spin_unlock(&ring_lock);
    }
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

    printk(KERN_INFO "midi_inject: receive=0x%lx register=0x%lx"
           " midi_out=0x%lx midi_rt=0x%lx\n",
           receive_fn, register_fn, midi_out_fn, midi_rt_fn);

    if (!receive_fn || !register_fn) {
        printk(KERN_ERR "midi_inject: receive_fn and register_fn required\n");
        return -EINVAL;
    }

    port_obj = find_port_object();
    if (!port_obj) return -ENODEV;

    ret = setup_ring();
    if (ret < 0)
        printk(KERN_WARNING "midi_inject: Block 5 ring setup failed (%d)\n", ret);
    else
        have_ring = 1;

    /* Install DIN MIDI OUT hooks if addresses provided */
    if (midi_out_fn) {
        if (install_hook(midi_out_fn, hook_send_single_byte,
                         &tramp_out, orig_out) == 0)
            have_ring = 1;
        else
            printk(KERN_WARNING "midi_inject: SendSingleByte hook failed\n");
    }
    if (midi_rt_fn) {
        if (install_hook(midi_rt_fn, hook_send_real_time,
                         &tramp_rt, orig_rt) == 0)
            have_ring = 1;
        else
            printk(KERN_WARNING "midi_inject: SendRealTime hook failed\n");
    }

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (!proc_midi_in) return -ENOMEM;
    proc_midi_in->write_proc = midi_write;

    if (have_ring) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;
    }

    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_inject: ready — port=%p blk5=%s din_hook=%s\n",
           port_obj,
           hw_data    ? "ok"  : "none",
           tramp_out  ? "ok"  : "none");
    return 0;
}

static void __exit midi_inject_exit(void)
{
    unregister_module_notifier(&midi_nb);

    /* Remove hooks before any in-flight calls can use freed trampoline memory.
     * synchronize_rcu() ensures all CPUs have left the hook path. */
    remove_hook(midi_out_fn, orig_out, &tramp_out);
    remove_hook(midi_rt_fn,  orig_rt,  &tramp_rt);
    synchronize_rcu();

    ring_disable();

    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_inject: unloaded\n");
}

module_init(midi_inject_init);
module_exit(midi_inject_exit);
