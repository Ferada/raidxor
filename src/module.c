#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include "raidxor.h"

static void raidxor_try_configure_raid(raidxor_conf_t *conf) {

}

/*
  everything < 4096 Bytes !

  /sys/md/raidxor/number_of_resources:
  [number_of_resources]

  /sys/md/raidxor/units_per_resource
  [number_of_units_per_resource]

  then we have a grid and can assign the units

  /sys/md/raidxor/redundancy:
  [unit_dev_t][redundant][length_of_equation]
    [unit_dev_t] ... [unit_dev_t]
  [unit_dev_t][not_redundant]
  ...
 */
static ssize_t
raidxor_show_units_per_resource(mddev_t *mddev, char *page)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (conf)
		return sprintf(page, "%ul\n", conf->units_per_resource);
	else
		return 0;
}

static ssize_t
raidxor_store_units_per_resource(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long new;
	int err;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	new = simple_strtoul(page, NULL, 10);

	if (new == 0)
		return -EINVAL;

	if (conf->resources != NULL) {
		kfree(conf->resources);
		conf->resources = NULL;
	}

	conf->units_per_resource = new;

	raidxor_try_configure_raid(conf);

	return 0;
}

static ssize_t
raidxor_show_number_of_resources(mddev_t *mddev, char *page)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (conf)
		return sprintf(page, "%ul\n", conf->n_resources);
	else
		return 0;
}

static ssize_t
raidxor_store_number_of_resources(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long new;
	int err;
	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	/* new_decode_dev can get us a dev_t from an encoded userland value
	   (minor, major) */
#if 0
	if (strict_strtoul(page, 10, &new))
		return -EINVAL;
	if (new <= 16 || new > 32768)
		return -EINVAL;
	while (new < conf->max_nr_stripes) {
		if (drop_one_stripe(conf))
			conf->max_nr_stripes--;
		else
			break;
	}
	err = md_allow_write(mddev);
	if (err)
		return err;
	while (new > conf->max_nr_stripes) {
		if (grow_one_stripe(conf))
			conf->max_nr_stripes++;
		else break;
	}
	return len;
#endif

	return -EINVAL;
}

static struct md_sysfs_entry
raidxor_number_of_resources = __ATTR(number_of_resources, S_IRUGO | S_IWUSR,
				     raidxor_show_number_of_resources,
				     raidxor_store_number_of_resources);

static struct md_sysfs_entry
raidxor_units_per_resource = __ATTR(units_per_resource, S_IRUGO | S_IWUSR,
				    raidxor_show_units_per_resource,
				    raidxor_store_units_per_resource);

static struct attribute *raidxor_attrs[] = {
	&raidxor_number_of_resources,
	&raidxor_units_per_resource,
	//&raidxor_encoding,
};
static struct attribute_group raidxor_attrs_group = {
	.name = NULL,
	.attrs = raidxor_attrs,
};

static void check_raid_parameters(raidxor_conf_t *conf)
{
	spin_lock(&conf->device_lock);
	conf->configured = 0;
	spin_unlock(&conf->device_lock);
}



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

	conf->configured = 0;
	conf->mddev = mddev;
	conf->units_per_resource = 0;
	conf->n_resoures = 0;
	conf->resources = NULL;
	conf->n_stripes = 0;
	conf->stripes = NULL;
	conf->n_units = mddev->raid_disks;

	spin_lock_init(&conf->device_lock);

	printk(KERN_INFO "raidxor: FIXME: assuming devices in linear order\n");

	size = -1; /* in sectors, that is 1024 byte (or 512? find out!)*/
	i = 0;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		printk(KERN_INFO "raidxor: rdev %s, %llu\n", bdevname(rdev->bdev, buffer),
			(unsigned long long) rdev->size);
#if 0
		conf->disks[i].rdev = rdev;
#endif

		++i;
	}
	if (size == -1)
		goto out_inval;
	mddev->size = size;
	mddev->array_sectors = size;

	printk (KERN_INFO "raidxor: array_sectors is %llu sectors\n",
		(unsigned long long) mddev->array_sectors);

	/* Ok, everything is just fine now */
	if (sysfs_create_group(&mddev->kobj, &raidxor_attrs_group)) {
		printk(KERN_ERR
		       "raidxor: failed to create sysfs attributes for %s\n",
		       mdname(mddev));
		goto out_free_conf;
	}

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

	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);
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
	raidxor_bio_t *rxbio = (raidxor_bio_t *)(bio->bi_private);
	raidxor_conf_t *conf = mddev_to_conf(rxbio->mddev);
	unsigned int i, j, index;
	struct bio *mbio = rxbio->master_bio;
	struct bio_vec *bvfrom, *bvto;
	int from_offset, to_offset;
	char *mapped;

#if 0
	for (index = 0; index < conf->n_data_disks; ++index) {
		if (conf->disks[index].rdev->bdev == bio->bi_bdev)
			break;
	}
#endif
	/* offset for the copy operations */
	to_offset = index * 512;

	/* the data which the master bio wants, is partially in the pages
	   of this bio, therefore we copy it */
	i = 0;
	bvto = bio_iovec_idx(mbio, i);
	for (; to_offset >= bvto->bv_len;) {
		to_offset -= bvto->bv_len;
		bvto = bio_iovec_idx(mbio, ++i);
	}

	/* copy chunks of 512 bytes, advancing bvfrom and bvto when
	   necessary */
	j = 0;
	from_offset = 0;
	bvfrom = bio_iovec_idx(bio, j);
	for (; i < bio->bi_vcnt;) {
		mapped = __bio_kmap_atomic(mbio, i, KM_USER0);
		memcpy(mapped + bvto->bv_offset + to_offset,
		       bvfrom->bv_page + bvfrom->bv_offset + from_offset,
		       512);
		__bio_kunmap_atomic(mbio, KM_USER0);

		from_offset += 512;
		if (from_offset >= bvfrom->bv_len)
			bvfrom = bio_iovec_idx(bio, ++j);

#if 0
		to_offset += conf->n_data_disks * 512;
#endif
		if (to_offset >= bvto->bv_len)
			bvto = bio_iovec_idx(mbio, ++i);
	}

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
	raidxor_bio_t *rxbio;
	int i, j;

	printk(KERN_INFO "raidxor: got request\n");

	goto out;

	printk(KERN_INFO "raidxor: splitting from sector %llu, %llu bytes\n",
	       (unsigned long long) bio->bi_sector, (unsigned long long) bio->bi_size);

	/* we don't handle read requests yet */
	/* apparently, we do have to handle them ... */
	if (rw == READ) {
		printk (KERN_INFO "raidxor: handling read request\n");

		/* TODO: create a pool for this */
		//rxbio = mempool_alloc(conf->rxbio_pool, GFP_NOIO);
#if 0
		rxbio = kzalloc(sizeof(raidxor_bio) +
				sizeof(struct bio *) * conf->n_data_disks, GFP_NOIO);
#endif
		if (!rxbio)
			goto out_free_rxbio;
		rxbio->master_bio = bio;
		rxbio->mddev = mddev;

#if 0
		/* reading at most one sector more then necessary on (each disk - 1) */
		size = 512 * ((bio->bi_size / 512) / conf->n_data_disks +
			      ((bio->bi_size / 512) % conf->n_data_disks) ? 1 : 0);
#endif
		npages = size / PAGE_SIZE + (size % PAGE_SIZE) ? 1 : 0;
		printk(KERN_INFO "raidxor: into requests of size %llu a %u pages\n",
		       (unsigned long long) size, npages);

		atomic_set(&rxbio->remaining, 0);

#if 0
		for (i = 0; i < conf->n_data_disks; ++i) {
			rbio = bio_alloc(GFP_NOIO, npages);
			if (!rbio)
				goto out_free_pages;
			rxbio->bios[i] = rbio;

			rbio->bi_rw = READ;
			rbio->bi_private = rxbio;

			rbio->bi_bdev = conf->disks[i].rdev->bdev;
			rbio->bi_sector = bio->bi_sector / conf->n_data_disks +
				conf->disks[i].rdev->data_offset;
			rbio->bi_size = size;

			printk(KERN_INFO "raidxor: request %d goes to physical sector %llu\n",
			       i, (unsigned long long) rbio->bi_sector);

			rbio->bi_end_io = raidxor_end_read_request;

			for (j = 0; j < npages; ++j) {
				page = alloc_page(GFP_NOIO);
				if (!page)
					goto out_free_pages;
				rbio->bi_io_vec[j].bv_page = page;
				rbio->bi_io_vec[j].bv_len = PAGE_SIZE;
				rbio->bi_io_vec[j].bv_offset = j * PAGE_SIZE;
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
#endif
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
