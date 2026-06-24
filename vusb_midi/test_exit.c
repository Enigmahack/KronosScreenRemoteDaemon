#include <linux/module.h>
#include <linux/init.h>
#include <linux/workqueue.h>

static struct work_struct test_work;

static void test_setup(struct work_struct *w)
{
	printk(KERN_INFO "test_exit: init ran in workqueue\n");
}

static int test_init(void)
{
	INIT_WORK(&test_work, test_setup);
	schedule_work(&test_work);
	return 0;
}

static void test_exit(void)
{
	flush_scheduled_work();
	printk(KERN_INFO "test_exit: cleanup ran\n");
}

MODULE_LICENSE("GPL");
module_init(test_init);
module_exit(test_exit);
