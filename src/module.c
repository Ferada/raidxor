#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include "raidxor.h"


static int raidxor_run(mddev_t *mddev)
{
	struct list_head *tmp;
	mdk_rdev_t* rdev;
	int disks = 0;

	printk("raidxor: run(%s) called.\n", mdname(mddev));

	rdev_for_each(rdev, tmp, mddev) {
		++disks;
	}

	printk("%d disks in configuration.\n", disks);

	return 0;
}

static int raidxor_stop(mddev_t *mddev)
{
	return 0;
}

static void raidxor_status(struct seq_file *seq, mddev_t *mddev)
{
	seq_printf(seq, " I'm feeling fine");
	return;
}


static struct mdk_personality raidxor_personality =
{
	.name   = "raidxor",
	.level  = LEVEL_XOR,
	.owner  = THIS_MODULE,
	/* .make_request = raidxor_make_request, */
	.run    = raidxor_run,
	.stop   = raidxor_stop,
	.status = raidxor_status,

	/* handles faulty disks, so we have to implement this one */
	/* .error_handler = raidxor_error, */

	/* .sync_request = raidxor_sync_request, */
	/* .quiesce = raidxor_quiesce, */
};


static int __init raidxor_init(void)
{
	return register_md_personality(&raidxor_personality);
}

static void __exit raidxor_exit(void)
{
	unregister_md_personality(&raidxor_personality);
}


module_init( raidxor_init );
module_exit( raidxor_exit );


MODULE_AUTHOR("Olof-Joachim Frahm");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("none");
MODULE_DESCRIPTION("Raid module with parameterisation support for en- and decoding.");
