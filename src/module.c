#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include "raidxor.h"


static int raidxor_run(mddev_t *mddev)
{
	conf_t *conf;
	struct list_head *tmp;
	mdk_rdev_t* rdev;

	rdev_for_each(rdev, tmp, mddev) {
		/* nuthin' */
	}

	if (mddev->level != LEVEL_XOR) {
		printk(KERN_ERR "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}


	conf = kzalloc(sizeof(conf_t), GFP_KERNEL);
	mddev->private = conf;
	if (!conf)
		goto out_no_mem;

	conf->mddev = mddev;


	printk(KERN_INFO "raidxor: raid set %s active with %d disks\n",
	       mdname(mddev), mddev->raid_disks);

	return 0;

out_no_mem:
	printk(KERN_ERR "raidxor: couldn't allocate memory for %s\n",
	       mdname(mddev));

out_free_conf:
	if (conf) {
		kfree(conf);
		mddev->private = NULL;
	}

out:
	return -EIO;

out_inval:
	return -EINVAL;
}

static int raidxor_stop(mddev_t *mddev)
{
	conf_t *conf = mddev_to_conf(mddev);

	mddev->private = NULL;
	kfree(conf);

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
