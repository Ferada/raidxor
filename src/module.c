#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include "raidxor.h"


static int raidxor_run(mddev_t *mddev)
{
	raidxor_conf_t *conf;
	struct list_head *tmp;
	mdk_rdev_t* rdev;
	char buffer[32];
	sector_t size;

	printk (KERN_INFO "ignoring mddev->chunk_size\n");

	size = -1;
	rdev_for_each(rdev, tmp, mddev) {
		/* nuthin' */
		
		printk(KERN_INFO "raidxor: rdev %s\n", bdevname(rdev->bdev, buffer));
		
		size = min(size, rdev->size);
	}
	if (size == -1)
		goto out_inval;
	mddev->array_size = size;

	printk (KERN_INFO "raidxor: md_size is %llu blocks\n",
		(unsigned long long) mddev->array_size);

	if (mddev->level != LEVEL_XOR) {
		printk(KERN_ERR "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}

	conf = kzalloc(sizeof(raidxor_conf_t), GFP_KERNEL);
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
		mddev_to_conf(mddev) = NULL;
	}
out:
	return -EIO;

out_inval:
	return -EINVAL;
}

static int raidxor_stop(mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	mddev_to_conf(mddev) = NULL;
	kfree(conf);

	return 0;
}

static void raidxor_status(struct seq_file *seq, mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	seq_printf(seq, " I'm feeling fine");
	return;
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio) {
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	const int rw = bio_data_dir(bio);

	printk (KERN_INFO "raidxor: got request\n");

	/* we don't handle read requests yet */
	if (rw == READ) {
		bio_endio(bio, -EOPNOTSUPP);
		return 0;
	}

	// only used for md driver housekeeping
	md_write_start(mddev, bio);

	//bio_endio(bio, -EOPNOTSUPP);

	// signal an read error to the md layer
	//md_error(

	// stop this transfer and signal an error to upper level (not md, but blk_queue)
	//bio_io_error(bio); // == bio_endio(bio, -EIO)
	return 0;
}

static struct mdk_personality raidxor_personality =
{
	.name         = "raidxor",
	.level        = LEVEL_XOR,
	.owner        = THIS_MODULE,

	.make_request = raidxor_make_request,
	.run          = raidxor_run,
	.stop         = raidxor_stop,
	.status       = raidxor_status,

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

MODULE_AUTHOR("Olof-Joachim Frahm");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("md");
MODULE_DESCRIPTION("Raid module with parameterisation support for en- and decoding.");

module_init( raidxor_init );
module_exit( raidxor_exit );
