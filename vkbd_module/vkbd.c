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
 */
 
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <asm/uaccess.h>

static struct input_dev      *vkbd_dev;
static struct proc_dir_entry *vkbd_proc;
static struct work_struct     vkbd_work;

static int vkbd_write_proc(struct file *file, const char __user *buf,
			   unsigned long count, void *data)
{
	char kbuf[32];
	unsigned int code = 0, value = 0;
	size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;

	if (!vkbd_dev)
		return -ENODEV;
	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';

	if (sscanf(kbuf, "%u %u", &code, &value) < 2 ||
	    code == 0 || code >= KEY_CNT || value > 1)
		return -EINVAL;

	input_event(vkbd_dev, EV_KEY, code, value);
	input_sync(vkbd_dev);
	return (int)count;
}

static void vkbd_setup(struct work_struct *work)
{
	int i, err;

	vkbd_dev = input_allocate_device();
	if (!vkbd_dev) {
		printk(KERN_ERR "vkbd: input_allocate_device failed\n");
		return;
	}

	vkbd_dev->name       = "Kronos Virtual Keyboard";
	vkbd_dev->id.bustype = BUS_USB;
	vkbd_dev->id.vendor  = 0x0001;
	vkbd_dev->id.product = 0x0001;
	vkbd_dev->id.version = 0x0001;

	set_bit(EV_KEY, vkbd_dev->evbit);
	set_bit(EV_SYN, vkbd_dev->evbit);
	for (i = 1; i < KEY_CNT; i++)
		set_bit(i, vkbd_dev->keybit);

	err = input_register_device(vkbd_dev);
	if (err) {
		printk(KERN_ERR "vkbd: input_register_device failed (%d)\n", err);
		input_free_device(vkbd_dev);
		vkbd_dev = NULL;
		return;
	}

	vkbd_proc = create_proc_entry(".vkbd", 0200, NULL);
	if (vkbd_proc)
		vkbd_proc->write_proc = vkbd_write_proc;

	printk(KERN_INFO "vkbd: %s\n",
	       vkbd_proc ? "ready - /proc/.vkbd" : "ready (proc entry failed)");
}

static int __init vkbd_init(void)
{
	INIT_WORK(&vkbd_work, vkbd_setup);
	schedule_work(&vkbd_work);
	return 0;
}

static void __exit vkbd_exit(void)
{
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
