/*
 * vkbd.c - Virtual keyboard for Kronos ScreenRemote
 *
 * Registers an input_dev via the kernel input subsystem so Eva hotplugs it
 * exactly like a physical USB keyboard.  No userspace device node needed.
 *
 * Key injection: write "code value\n" to /proc/.vkbd
 *   e.g.  echo "30 1" > /proc/.vkbd   (KEY_A press)
 *         echo "30 0" > /proc/.vkbd   (KEY_A release)
 *
 * RTAI note: GFP_KERNEL and create_proc_entry both fail when called directly
 * from init_module on this kernel.  All setup runs via schedule_work().
 *
 * Relay to whatever Eva is actually bound to
 * --------------------------------------------
 * Eva's CHIDDriver::KeyboardIsConnected() (decompiled from Eva 3.2.2,
 * eva_export/functions/KeyboardIsConnected@08e4fb20.c) tracks exactly ONE
 * keyboard fd: it scans /sys/class/input/event0..event3 for the first device
 * whose id/bustype sysfs attribute reads "0003" (BUS_USB) - no EV_KEY check,
 * so ANY USB input device in that range qualifies, not just keyboards - and
 * locks onto it until that device's sysfs node disappears.  It never rescans
 * while its bound fd stays valid - crucially, that includes never rescanning
 * just because some OTHER device shows up. It only re-evaluates anything when
 * its currently-bound device's sysfs node disappears.
 *
 * vkbd.ko loads early (before Eva starts, see screenremote.c's main()) and
 * sets id.bustype = BUS_USB specifically so this check recognizes it. That
 * works when vkbd is the only such device around. But if a real USB keyboard
 * is already attached at power-on, the kernel's USB/HID stack enumerates it
 * during early boot - before our daemon even starts - so it claims a lower
 * /dev/input/eventN slot than vkbd. Eva's scan then finds and locks onto the
 * real keyboard first; vkbd, pushed out past Eva's scanned range, is never
 * even looked at. Client-injected keystrokes land correctly in vkbd's own
 * input_dev, but Eva is reading a different device entirely - invisible.
 *
 * Rather than fight Eva for a slot (impossible to win reliably - a keyboard
 * already attached at boot always registers before we do, and Eva doesn't
 * rescan once bound), this module tracks, via a standard input_handler
 * (vkbd_relay_* below), whichever device we believe Eva is currently bound
 * to - real or vkbd_dev itself - and injects into THAT device. Since
 * input_event() delivers to every handler currently attached to a device
 * (evdev included, which is what Eva reads through), this reaches Eva no
 * matter which single device it happens to be bound to, without ever
 * displacing a real keyboard, which keeps working as itself throughout.
 *
 * Sticky target selection - the part that's easy to get wrong
 * ---------------------------------------------------------------
 * A first version of this preferred "any connected external device" over
 * vkbd_dev unconditionally. That's wrong: once vkbd_dev has *already* become
 * what Eva is bound to (see slot reclaim below), a real keyboard plugging in
 * later must NOT preempt it as our injection target - Eva only rescans when
 * its current device disappears, never just because another one appears.
 * Confirmed live 2026-07-19: after a successful reclaim, a real keyboard was
 * plugged back in, our relay switched its injection target to it (matching
 * the old, wrong "prefer external" rule) - but Eva stayed bound to vkbd_dev
 * (which hadn't gone anywhere), so from that point neither the real keyboard
 * (typing landed on a device Eva wasn't reading) nor vkbd (same reason)
 * reached Eva, though both still worked fine as ordinary input devices (e.g.
 * to the local console) throughout.
 *
 * The fix: target selection is sticky. vkbd_current_ext (NULL meaning
 * "target is vkbd_dev itself") is only ever set in two places: (a) during
 * the one-time initial device scan inside input_register_handler(), called
 * from vkbd_setup() before Eva has even started - gated by
 * vkbd_initial_scan_done, so it only applies to devices already present at
 * that moment, mirroring whatever Eva's own first scan is about to find;
 * and (b) implicitly via slot reclaim, whenever the device vkbd_current_ext
 * points at itself disconnects. A plain hotplug connect() happening any
 * other time is recorded (so disconnect() bookkeeping stays correct) but
 * never changes the active target - exactly mirroring Eva's own "never
 * rescans just because something new showed up" behavior.
 *
 * The id_table below matches on bustype alone (no EV_KEY requirement),
 * deliberately mirroring Eva's own loose check - including its quirk that a
 * USB mouse in event0..3 would equally be mistaken for "the keyboard" by
 * Eva itself. We're matching Eva's actual behavior here, not fixing it.
 *
 * Slot reclaim on last external departure
 * ------------------------------------------
 * Falling back to vkbd_dev when no external candidate remains isn't enough
 * on its own: vkbd_dev registered once, at module load, and never moves -
 * if that happened while a real keyboard already held Eva's scanned range,
 * vkbd's own slot is permanently outside it too, real keyboard or not.
 * vkbd_reclaim_fn() below unregisters and re-registers vkbd_dev the moment
 * the actively-targeted external device disconnects and no other external
 * candidate remains, so it can claim whichever slot just vacated - exactly
 * the slot Eva's own rescan (on noticing its bound device vanished) is about
 * to go looking in.
 *
 * This has to run from a workqueue, not directly from vkbd_relay_disconnect().
 * input_unregister_device()/input_register_device() both take the kernel's
 * global input_mutex, and disconnect() callbacks are themselves invoked
 * while that same mutex is already held (input_unregister_device() on the
 * departing device holds it across every attached handler's disconnect()
 * call) - doing either synchronously from inside our own disconnect() would
 * self-deadlock on that mutex.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

static struct input_dev      *vkbd_dev;
static struct proc_dir_entry *vkbd_proc;
static struct work_struct     vkbd_work;

/* Bookkeeping: one of these per currently-connected external (non-vkbd)
 * BUS_USB input device, so disconnect() can always find and remove the
 * right one. Only vkbd_current_ext (if non-NULL) is actually the active
 * injection target - see the big sticky-target comment above. */
struct vkbd_ext_dev {
	struct input_handle handle;
	struct list_head     node;
};

static LIST_HEAD(vkbd_ext_list);
static DEFINE_SPINLOCK(vkbd_target_lock);
static struct vkbd_ext_dev *vkbd_current_ext;   /* NULL => target is vkbd_dev */
static int vkbd_initial_scan_done;
static int vkbd_relay_registered;
static struct work_struct vkbd_reclaim_work;

/* Allocates, configures, and registers a fresh "Kronos Virtual Keyboard"
 * input_dev, publishing it as the new vkbd_dev.  Shared by the initial
 * setup and by vkbd_reclaim_fn() - identical device shape either time.
 * Must run from process context with input_mutex NOT held by the caller
 * (input_register_device() takes it) - both callers satisfy this: vkbd_setup()
 * runs from its own schedule_work(), vkbd_reclaim_fn() from its own. */
static int vkbd_register_self(void)
{
	struct input_dev *dev;
	int i, err;

	dev = input_allocate_device();
	if (!dev) {
		printk(KERN_ERR "vkbd: input_allocate_device failed\n");
		return -ENOMEM;
	}

	dev->name       = "Kronos Virtual Keyboard";
	dev->id.bustype = BUS_USB;
	dev->id.vendor  = 0x0001;
	dev->id.product = 0x0001;
	dev->id.version = 0x0001;

	set_bit(EV_KEY, dev->evbit);
	set_bit(EV_SYN, dev->evbit);
	for (i = 1; i < KEY_CNT; i++)
		set_bit(i, dev->keybit);

	/* Publish before registering: input_register_device() below will
	 * synchronously invoke our own relay handler's connect() (our
	 * id_table matches BUS_USB and this device qualifies), which checks
	 * dev == vkbd_dev to exclude itself - it needs to see the new pointer
	 * already in place to do that correctly. */
	spin_lock(&vkbd_target_lock);
	vkbd_dev = dev;
	spin_unlock(&vkbd_target_lock);

	err = input_register_device(dev);
	if (err) {
		printk(KERN_ERR "vkbd: input_register_device failed (%d)\n", err);
		spin_lock(&vkbd_target_lock);
		vkbd_dev = NULL;
		spin_unlock(&vkbd_target_lock);
		input_free_device(dev);
		return err;
	}

	return 0;
}

static void vkbd_reclaim_fn(struct work_struct *work)
{
	struct input_dev *old;

	spin_lock(&vkbd_target_lock);
	if (vkbd_current_ext) {
		/* A new external target got adopted before this ran (only
		 * possible via the reclaim path itself finishing elsewhere -
		 * plain hotplugs never adopt post-initial-scan) - nothing to
		 * reclaim. */
		spin_unlock(&vkbd_target_lock);
		return;
	}
	old = vkbd_dev;
	vkbd_dev = NULL;
	spin_unlock(&vkbd_target_lock);

	if (old)
		input_unregister_device(old);

	if (vkbd_register_self() == 0)
		printk(KERN_INFO "vkbd: reclaimed a fresh slot after external "
		       "keyboard departure\n");
}

static void vkbd_relay_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	/* Never opened (see connect()), so handle->open stays 0 and the input
	 * core never actually calls this - kept only as a safety net against
	 * a NULL handler->event deref if that invariant ever changes. */
}

static int vkbd_relay_connect(struct input_handler *handler, struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct vkbd_ext_dev *ext;
	int err;
	int is_self;

	spin_lock(&vkbd_target_lock);
	is_self = (dev == vkbd_dev);
	spin_unlock(&vkbd_target_lock);
	if (is_self)
		return -ENODEV;  /* never track ourselves as an "external" target */

	ext = kzalloc(sizeof(*ext), GFP_KERNEL);
	if (!ext)
		return -ENOMEM;

	ext->handle.dev     = dev;
	ext->handle.handler = handler;
	ext->handle.name    = "vkbd_relay";

	err = input_register_handle(&ext->handle);
	if (err) {
		kfree(ext);
		return err;
	}

	spin_lock(&vkbd_target_lock);
	list_add_tail(&ext->node, &vkbd_ext_list);
	/* Only adopt as the active target during the one-time initial scan
	 * (devices already present when we loaded, mirroring whatever Eva's
	 * own first scan is about to find) and only if nothing's already
	 * claimed the spot. A later hotplug connect() must never preempt an
	 * established target - see the sticky-target comment up top. */
	if (!vkbd_initial_scan_done && !vkbd_current_ext)
		vkbd_current_ext = ext;
	spin_unlock(&vkbd_target_lock);

	printk(KERN_INFO "vkbd: relay target available - %s (bus %04x)\n",
	       dev_name(&dev->dev), dev->id.bustype);
	return 0;
}

static void vkbd_relay_disconnect(struct input_handle *handle)
{
	struct vkbd_ext_dev *ext = container_of(handle, struct vkbd_ext_dev, handle);
	int need_reclaim = 0;

	spin_lock(&vkbd_target_lock);
	list_del(&ext->node);
	if (vkbd_current_ext == ext) {
		/* The device we were actually relaying into just vanished -
		 * this is the one moment Eva itself would also rescan. Prefer
		 * another already-connected external if one exists (best
		 * effort at matching whatever Eva's rescan would find);
		 * otherwise fall back to vkbd_dev via reclaim. */
		if (!list_empty(&vkbd_ext_list))
			vkbd_current_ext = list_first_entry(&vkbd_ext_list,
							     struct vkbd_ext_dev, node);
		else {
			vkbd_current_ext = NULL;
			need_reclaim = 1;
		}
	}
	spin_unlock(&vkbd_target_lock);

	input_unregister_handle(handle);
	printk(KERN_INFO "vkbd: relay target gone - %s\n", dev_name(&handle->dev->dev));
	kfree(ext);

	/* Deferred to a workqueue - see vkbd_reclaim_fn()'s comment for why
	 * this can't run directly here. */
	if (need_reclaim)
		schedule_work(&vkbd_reclaim_work);
}

static const struct input_device_id vkbd_relay_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_BUS, .bustype = BUS_USB },
	{ },
};

static struct input_handler vkbd_relay_handler = {
	.event      = vkbd_relay_event,
	.connect    = vkbd_relay_connect,
	.disconnect = vkbd_relay_disconnect,
	.name       = "vkbd_relay",
	.id_table   = vkbd_relay_ids,
};

static int vkbd_write_proc(struct file *file, const char __user *buf,
			   unsigned long count, void *data)
{
	char kbuf[32];
	unsigned int code = 0, value = 0;
	size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
	struct input_dev *target;

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';

	if (sscanf(kbuf, "%u %u", &code, &value) < 2 ||
	    code == 0 || code >= KEY_CNT || value > 1)
		return -EINVAL;

	spin_lock(&vkbd_target_lock);
	target = vkbd_current_ext ? vkbd_current_ext->handle.dev : vkbd_dev;
	if (!target) {
		spin_unlock(&vkbd_target_lock);
		return -ENODEV;
	}
	input_event(target, EV_KEY, code, value);
	input_sync(target);
	spin_unlock(&vkbd_target_lock);

	return (int)count;
}

static void vkbd_setup(struct work_struct *work)
{
	int err;

	if (vkbd_register_self() != 0)
		return;

	/* Must come after vkbd_dev is set (inside vkbd_register_self()) -
	 * connect()'s self-exclusion check (dev == vkbd_dev) needs it,
	 * including during this call's own initial scan of already-registered
	 * devices - that initial scan is also the one window in which
	 * connect() is allowed to adopt an external target (see the
	 * sticky-target comment up top), so vkbd_initial_scan_done must not
	 * be set until this returns. */
	err = input_register_handler(&vkbd_relay_handler);
	if (err)
		printk(KERN_ERR "vkbd: relay handler registration failed (%d) - "
		       "falling back to always injecting into vkbd itself\n", err);
	else
		vkbd_relay_registered = 1;

	spin_lock(&vkbd_target_lock);
	vkbd_initial_scan_done = 1;
	spin_unlock(&vkbd_target_lock);

	vkbd_proc = create_proc_entry(".vkbd", 0200, NULL);
	if (vkbd_proc)
		vkbd_proc->write_proc = vkbd_write_proc;

	printk(KERN_INFO "vkbd: %s\n",
	       vkbd_proc ? "ready - /proc/.vkbd" : "ready (proc entry failed)");
}

static int __init vkbd_init(void)
{
	INIT_WORK(&vkbd_work, vkbd_setup);
	INIT_WORK(&vkbd_reclaim_work, vkbd_reclaim_fn);
	schedule_work(&vkbd_work);
	return 0;
}

static void __exit vkbd_exit(void)
{
	/* Unregister the relay handler FIRST, before any flush. Real hotplug
	 * disconnect()s can call schedule_work(&vkbd_reclaim_work) at any
	 * time up until the handler is detached - a flush_scheduled_work()
	 * done only up front (the original, crashing version of this
	 * function) can return clean and then race a disconnect() landing a
	 * moment later, scheduling reclaim work AFTER the flush already
	 * finished. If the module unload proceeds to completion before that
	 * freshly-scheduled work runs, the kernel later jumps into
	 * vkbd_reclaim_fn() inside memory that's already been freed -
	 * confirmed live 2026-07-19: a real captured oops in device_del()/
	 * sysfs_remove_group(), from a kernel workqueue thread, right after
	 * "last unloaded: vkbd" - textbook module-unload-vs-pending-work
	 * use-after-free. input_unregister_handler() detaches us from every
	 * device and removes us from the global handler list, so once it
	 * returns, connect()/disconnect() can never fire for us again -
	 * nothing can schedule new reclaim work after this point. The one
	 * flush below only has to wait out whatever's already in flight,
	 * including the synchronous disconnect() calls input_unregister_handler()
	 * itself just made for any devices still attached when we got here. */
	if (vkbd_relay_registered)
		input_unregister_handler(&vkbd_relay_handler);
	flush_scheduled_work();
	if (vkbd_proc)
		remove_proc_entry(".vkbd", NULL);
	if (vkbd_dev)
		input_unregister_device(vkbd_dev);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual keyboard for Kronos ScreenRemote");
module_init(vkbd_init);
module_exit(vkbd_exit);
