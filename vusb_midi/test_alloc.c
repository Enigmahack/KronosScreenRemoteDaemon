#include <linux/module.h>
#include <linux/vmalloc.h>
MODULE_LICENSE("GPL");
static int test_init(void) {
	void *p = vmalloc(64);
	printk(KERN_INFO "test_alloc: vmalloc(64) = %p\n", p);
	if (p) vfree(p);
	return 0;
}
static void test_exit(void) {}
module_init(test_init);
module_exit(test_exit);
