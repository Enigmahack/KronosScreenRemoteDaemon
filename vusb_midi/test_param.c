#include <linux/module.h>
MODULE_LICENSE("GPL");
static unsigned long testval;
module_param(testval, ulong, 0);
static int test_init(void) {
	printk(KERN_INFO "test_param: testval=0x%lx\n", testval);
	return 0;
}
static void test_exit(void) {}
module_init(test_init);
module_exit(test_exit);
