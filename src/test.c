#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

static int __init mod_init(void)
{
	printk("init_module called\n");
	return 0;
}

static void __exit mod_exit(void)
{
	printk("cleanup_module called\n");
}

MODULE_AUTHOR("Olof-Joachim Frahm");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A test module without any functionality.");
MODULE_SUPPORTED_DEVICE("none");

module_init( mod_init );
module_exit( mod_exit );
