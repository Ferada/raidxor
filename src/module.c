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
	unsigned int i;

	printk (KERN_INFO "raidxor: ignoring mddev->chunk_size\n");

	if (mddev->level != LEVEL_XOR) {
		printk(KERN_ERR "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}

	printk(KERN_INFO "raidxor: raid set %s active with %d disks\n",
	       mdname(mddev), mddev->raid_disks);

	printk(KERN_INFO "raidxor: FIXME: assuming two redundancy devices\n");
	if (mddev->raid_disks < 3)
		goto out_inval;

	conf = kzalloc(sizeof(raidxor_conf_t) +
		       sizeof(struct disk_info) * mddev->raid_disks, GFP_KERNEL);
	mddev->private = conf;
	if (!conf)
		goto out_no_mem;

	conf->mddev = mddev;
	conf->n_data_disks = (mddev->raid_disks - 2); /* FIXME */

	spin_lock_init(&conf->device_lock);

	printk(KERN_INFO "raidxor: FIXME: assuming devices in linear order\n");

	size = -1; /* in sectors, that is 1024 byte (or 512? find out!)*/
	i = 0;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		printk(KERN_INFO "raidxor: rdev %s, %llu\n", bdevname(rdev->bdev, buffer),
			(unsigned long long) rdev->size);
		conf->disks[i].rdev = rdev;

		++i;
	}
	if (size == -1)
		goto out_inval;
	mddev->size = size;
	mddev->array_sectors = size;

	printk (KERN_INFO "raidxor: array_sectors is %llu sectors\n",
		(unsigned long long) mddev->array_sectors);

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

static void raidxor_end_read_request(struct bio *bio, int error)
{
	raidxor_bio *rxbio = (raidxor_bio *)(bio->bi_private);
	raidxor_conf_t *conf = mddev_to_conf(rxbio->mddev);
	int i;

	if (atomic_dec_and_test(&rxbio->remaining)) {
		bio_endio(rxbio->master_bio, 0);
		/* TODO: create pool for this */
		//mempool_free(rxbio, conf->rxbio_pool);
		kfree(rxbio);
	}

	for (i = 0; i < bio->bi_vcnt; ++i)
		safe_put_page(bio->bi_io_vec[i].bv_page);

	bio_put(bio);
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio) {
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	const int rw = bio_data_dir(bio);
	unsigned int npages, size;
	struct bio *rbio;
	struct page *page;
	unsigned long flags;
	raidxor_bio *rxbio;
	int i, j;

	printk(KERN_INFO "raidxor: got request\n");

	printk(KERN_INFO "raidxor: splitting from sector %llu, %llu bytes\n",
	       (unsigned long long) bio->bi_sector, (unsigned long long) bio->bi_size);

	/* we don't handle read requests yet */
	/* apparently, we do have to handle them ... */
	if (rw == READ) {
		printk (KERN_INFO "raidxor: handling read request\n");

		/* TODO: create a pool for this */
		//rxbio = mempool_alloc(conf->rxbio_pool, GFP_NOIO);
		rxbio = kzalloc(sizeof(raidxor_bio) +
				sizeof(struct bio *) * conf->n_data_disks, GFP_NOIO);
		if (!rxbio)
			goto out_free_rxbio;
		rxbio->master_bio = bio;
		rxbio->mddev = mddev;

		/* reading at most one sector more then necessary on (each disk - 1) */
		size = 512 * ((bio->bi_size / 512) / conf->n_data_disks
			      + ((bio->bi_size / 512) % conf->n_data_disks) ? 1 : 0);
		npages = size / PAGE_SIZE + (size % PAGE_SIZE) ? 1 : 0;
		printk(KERN_INFO "raidxor: into requests of size %llu a %u pages\n",
		       (unsigned long long) size, npages);

		atomic_set(&rxbio->remaining, 0);

		for (i = 0; i < conf->n_data_disks; ++i) {
			rbio = bio_alloc(GFP_NOIO, npages);
			if (!rbio)
				goto out_free_pages;
			rxbio->bios[i] = rbio;

			rbio->bi_rw = READ;
			rbio->bi_private = rxbio;

			rbio->bi_bdev = conf->disks[i].rdev->bdev;
			rbio->bi_sector = bio->bi_sector / conf->n_data_disks;
			rbio->bi_size = size;

			printk(KERN_INFO "raidxor: request %d goes to physical sector %llu\n",
			       i, (unsigned long long) rbio->bi_sector);

			rbio->bi_end_io = raidxor_end_read_request;

			for (j = 0; j < npages; ++j) {
				page = alloc_page(GFP_NOIO);
				if (!page)
					goto out_free_pages;
				rbio->bi_io_vec[j].bv_page = page;
			}
		}

		for (i = 0; i < conf->n_data_disks; ++i) {
			atomic_inc(&rxbio->remaining);
			generic_make_request(rxbio->bios[i]);
		}

		return 0;
	out_free_pages:
		for (i = 0; i < conf->n_data_disks; ++i)
			if (rxbio->bios[i]) {
				for (j = 0; j < npages; ++j)
					safe_put_page(rxbio->bios[i]->bi_io_vec[j].bv_page);
				bio_put(rxbio->bios[i]);
			}
	out_free_rxbio:
		kfree(rxbio);

		goto out;
	}

	/* only used for md driver housekeeping */
	//md_write_start(mddev, bio);

	/* allocate enough memory for the transfers to each of the disks */
	/* size = (bi_size / (512 * data_disks)) */
	//stripe_size = 512 * ((bio->bi_size / 512) / conf->n_data_disks
	//		     + (bio->bi_size / 512) % conf->n_data_disks);
	
	//printk (KERN_INFO "raidxor: i want to allocate %u bytes for each drive\n", stripe_size);

	/* calculate the stripes */
	{
		
	}

	//bio_endio(bio, -EOPNOTSUPP);

	// signal an read error to the md layer
	//md_error(

	// stop this transfer and signal an error to upper level (not md, but blk_queue)
	//bio_io_error(bio); // == bio_endio(bio, -EIO)

out:
	bio_io_error(bio);
	return 0;
}

#if 0
static void raidxord(mddev_t *mddev) {
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	int unplug = 0;

	for (;;) {
		flush_pending_io(conf);
	}
}
#endif

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
