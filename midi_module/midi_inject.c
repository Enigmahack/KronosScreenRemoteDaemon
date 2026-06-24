/*
 * midi_inject.ko — MIDI injection + ring reader for Korg Kronos
 *
 * /proc/.midi_in  — write raw MIDI bytes to inject into OA.ko
 * /proc/.midi_ring — read SysEx responses from Block 5 at kernel speed
 *
 * Block 5 (128B) carries SysEx responses.  We locate it via the
 * container (Block 3) in shared memory and expose a /proc reader
 * that accesses the KVA directly — faster than userspace mmap.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>

MODULE_LICENSE("GPL");

static unsigned long receive_fn = 0;
module_param(receive_fn, ulong, 0444);

static unsigned long register_fn = 0;
module_param(register_fn, ulong, 0444);

typedef void (*receive_fn_t)(void *, const uint8_t *, uint32_t)
    __attribute__((regparm(3)));

static void *port_obj;
static uint32_t *ports_array;
static struct proc_dir_entry *proc_midi_in;
static struct proc_dir_entry *proc_midi_ring;
static uint8_t midi_buf[4096];

/* Block 5 ring state */
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

static int midi_module_notify(struct notifier_block *nb,
                              unsigned long action, void *data)
{
    struct module *mod = data;
    if (action == MODULE_STATE_GOING &&
        (strcmp(mod->name, "OA") == 0 ||
         strcmp(mod->name, "loadmod") == 0)) {
        ring_disable();
        printk(KERN_INFO "midi_inject: %s unloading, disabled\n", mod->name);
    }
    return NOTIFY_OK;
}

static struct notifier_block midi_nb = {
    .notifier_call = midi_module_notify,
};

/* ------------------------------------------------------------------ */
/*  Port discovery                                                     */
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
/*  Block 5 ring setup                                                 */
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

    /* Block 5 data KVA = Block 4 data KVA - 0x80 (physically adjacent) */
    blk4_data_kva = *(uint32_t *)((uint8_t *)(unsigned long)ports_array[0] + 0x104);
    hw_data = (uint8_t *)(unsigned long)(blk4_data_kva - 0x80);

    ring_cursor = q[3];

    printk(KERN_INFO "midi_inject: blk5 data=%p mask=0x%x\n", hw_data, hw_mask);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/.midi_ring — kernel-speed Block 5 reader                     */
/* ------------------------------------------------------------------ */

static ssize_t ring_fops_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint32_t wpos, avail;
    uint32_t start, first;
    size_t len;
    uint8_t tmp[512];
    uint8_t  *data;
    uint32_t *queue;
    uint32_t mask;

    spin_lock(&ring_lock);
    if (ring_dead || !hw_data || !hw_queue) {
        spin_unlock(&ring_lock);
        return -ENODEV;
    }
    data  = hw_data;
    queue = hw_queue;
    mask  = hw_mask;
    spin_unlock(&ring_lock);

    if (probe_kernel_read(&wpos, &queue[3], sizeof(wpos))) {
        ring_disable();
        printk(KERN_WARNING "midi_inject: ring memory gone (queue read)\n");
        return -ENODEV;
    }

    avail = wpos - ring_cursor;
    if (avail == 0)
        return 0;

    if (avail > (mask + 1))
        avail = mask + 1;

    len = avail;
    if (len > count) len = count;
    if (len > sizeof(tmp)) len = sizeof(tmp);

    start = ring_cursor & mask;
    first = (mask + 1) - start;
    if (first > (uint32_t)len) first = (uint32_t)len;

    if (probe_kernel_read(tmp, &data[start], first)) {
        ring_disable();
        printk(KERN_WARNING "midi_inject: ring memory gone (data read)\n");
        return -ENODEV;
    }
    if (first < (uint32_t)len) {
        if (probe_kernel_read(tmp + first, &data[0], len - first)) {
            ring_disable();
            return -ENODEV;
        }
    }

    if (copy_to_user(buf, tmp, len))
        return -EFAULT;

    ring_cursor += len;
    return len;
}

static ssize_t ring_fops_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    uint32_t *queue;
    uint32_t wpos;

    spin_lock(&ring_lock);
    if (ring_dead || !hw_queue) {
        spin_unlock(&ring_lock);
        return count;
    }
    queue = hw_queue;
    spin_unlock(&ring_lock);

    if (!probe_kernel_read(&wpos, &queue[3], sizeof(wpos)))
        ring_cursor = wpos;

    return count;
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

    printk(KERN_INFO "midi_inject: receive=0x%lx register=0x%lx\n",
           receive_fn, register_fn);

    if (!receive_fn || !register_fn) {
        printk(KERN_ERR "midi_inject: receive_fn and register_fn required\n");
        return -EINVAL;
    }

    port_obj = find_port_object();
    if (!port_obj) return -ENODEV;

    ret = setup_ring();
    if (ret < 0)
        printk(KERN_WARNING "midi_inject: ring setup failed (%d)\n", ret);

    proc_midi_in = create_proc_entry(".midi_in", 0666, NULL);
    if (!proc_midi_in) return -ENOMEM;
    proc_midi_in->write_proc = midi_write;

    if (hw_data) {
        proc_midi_ring = create_proc_entry(".midi_ring", 0666, NULL);
        if (proc_midi_ring)
            proc_midi_ring->proc_fops = &ring_fops;
    }

    register_module_notifier(&midi_nb);

    printk(KERN_INFO "midi_inject: ready — port=%p ring=%s\n",
           port_obj, hw_data ? "KVA" : "none");
    return 0;
}

static void __exit midi_inject_exit(void)
{
    unregister_module_notifier(&midi_nb);
    ring_disable();

    if (proc_midi_ring)
        remove_proc_entry(".midi_ring", NULL);
    if (proc_midi_in)
        remove_proc_entry(".midi_in", NULL);

    printk(KERN_INFO "midi_inject: unloaded\n");
}

module_init(midi_inject_init);
module_exit(midi_inject_exit);
