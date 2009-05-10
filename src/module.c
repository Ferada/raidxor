#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include "raidxor.h"

#define LOCKCONF(conf) \
	spin_lock(&conf->device_lock)

#define UNLOCKCONF(conf) \
	spin_unlock(&conf->device_lock)

#define WITHLOCKCONF(conf,block) \
	LOCKCONF(conf); \
	do block while(0); \
	UNLOCKCONF(conf);

/**
 * raidxor_safe_free_conf() - frees resource and stripe information
 *
 * Must be called inside conf lock.
 */
static void raidxor_safe_free_conf(raidxor_conf_t *conf) {
	if (!conf) {
		printk(KERN_DEBUG "raidxor: NULL pointer in raidxor_safe_free_conf\n");
		return;
	}

	if (conf->resources != NULL) {
		kfree(conf->resources);
		conf->resources = NULL;
	}

	if (conf->stripes != NULL) {
		kfree(conf->stripes);
		conf->stripes = NULL;
	}
	
	conf->configured = 0;
}

static void raidxor_try_configure_raid(raidxor_conf_t *conf) {
	raidxor_resource_t *resources;
	stripe_t *stripes;
	disk_info_t *unit;
	unsigned long i, j;
	char buffer[32];
	mddev_t *mddev = conf->mddev;
	sector_t size;

	/* new_decode_dev can get us a dev_t from an encoded userland value
	   (minor, major) */
	if (!conf) {
		printk(KERN_DEBUG "raidxor: NULL pointer in raidxor_free_conf\n");
		return;
	}

	if (conf->n_resources <= 0 || conf->units_per_resource <= 0) {
		printk(KERN_INFO
			"raidxor: need number of resources or units per resource: %lu or %lu\n",
			conf->n_resources, conf->units_per_resource);
		goto out;
	}

	if (conf->n_resources * conf->units_per_resource != conf->n_units) {
		printk(KERN_INFO
		       "raidxor: parameters don't match %lu * %lu != %lu\n",
		       conf->n_resources, conf->units_per_resource, conf->n_units);
		goto out;
	}

	for (i = 0; i < conf->n_units; ++i) {
		if (conf->units[i].redundant == -1) {
			printk(KERN_INFO
			       "raidxor: unit %lu, %s is not initialized\n",
			       i, bdevname(conf->units[i].rdev->bdev, buffer));
			goto out;
		}
	}

	printk(KERN_INFO "raidxor: got enough information, building raid\n");

	resources = kzalloc((sizeof(raidxor_resource_t) +
			     sizeof(disk_info_t *) * conf->units_per_resource) *
			    conf->n_resources, GFP_KERNEL);
	if (!resources)
		goto out;

	conf->n_stripes = conf->units_per_resource;

	stripes = kzalloc((sizeof(stripe_t) +
			   sizeof(disk_info_t *) * conf->n_resources) *
			  conf->n_stripes, GFP_KERNEL);
	if (!stripes)
		goto out_free_res;

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i].n_units = conf->units_per_resource;
		for (j = 0; j < conf->units_per_resource; ++j) {
			unit = &conf->units[i + j * conf->n_resources];

			unit->resource = &resources[i];
			resources[i].units[j] = unit;
		}
	}

	for (i = 0; i < conf->n_resources; ++i) {
		stripes[i].n_units = conf->n_resources;
		stripes[i].size = 0;

		for (j = 0; j < conf->n_resources; ++j) {
			unit = &conf->units[i * conf->n_resources + j];

			unit->stripe = &stripes[i];
			if (unit->redundant == 0) {
				++stripes[i].n_data_units;
				stripes[i].size += unit->rdev->size * 2;
			}
			stripes[i].units[j] = unit;
		}
		size += stripes[i].size / 2;
	}

	/* FIXME: size must be size * n_data_disks or something */
	/* device size must a multiple of chunk size */
	mddev->array_sectors = size;

	printk (KERN_INFO "raidxor: array_sectors is %llu blocks\n",
		(unsigned long long) mddev->array_sectors * 2);

	conf->resources = resources;
	conf->stripes = stripes;
	conf->configured = 1;

	return;
out_free_res:
	kfree(resources);
out:
	return;
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
		return sprintf(page, "%lu\n", conf->units_per_resource);
	else
		return -ENODEV;
}

static ssize_t
raidxor_store_units_per_resource(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long new;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	strict_strtoul(page, 10, &new);

	if (new == 0)
		return -EINVAL;

	WITHLOCKCONF(conf, {
	raidxor_safe_free_conf(conf);
	conf->units_per_resource = new;
	raidxor_try_configure_raid(conf);
	});

	return len;
}

static ssize_t
raidxor_show_number_of_resources(mddev_t *mddev, char *page)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (conf)
		return sprintf(page, "%lu\n", conf->n_resources);
	else
		return -ENODEV;
}

static ssize_t
raidxor_store_number_of_resources(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long new;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	strict_strtoul(page, 10, &new);

	if (new == 0)
		return -EINVAL;

	WITHLOCKCONF(conf, {
	raidxor_safe_free_conf(conf);
	conf->n_resources = new;
	raidxor_try_configure_raid(conf);
	});

	return len;
}

static ssize_t
raidxor_show_encoding(mddev_t *mddev, char *page)
{
	return -EIO;
}

static ssize_t
raidxor_store_encoding(mddev_t *mddev, const char *page, size_t len)
{
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

static struct md_sysfs_entry
raidxor_encoding = __ATTR(encoding, S_IRUGO | S_IWUSR,
			  raidxor_show_encoding,
			  raidxor_store_encoding);

static struct attribute *raidxor_attrs[] = {
	(struct attribute *) &raidxor_number_of_resources,
	(struct attribute *) &raidxor_units_per_resource,
	(struct attribute *) &raidxor_encoding,
	NULL
};

static struct attribute_group raidxor_attrs_group = {
	.name = NULL,
	.attrs = raidxor_attrs,
};



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

/**
 * raidxor_prepare_read_bio() - build several bios from one request
 *
 * The basic assumption is that we don't cross a stripe boundary (which
 * will be enforced by another function).
 *
 * We split according to mddev->chunk_size.
 */
static int raidxor_prepare_read_bio(raidxor_conf_t *conf, raidxor_bio_t *rxbio)
{
	unsigned long i, j;
	struct bio *mbio, *rbio;
	struct page *page;
	unsigned long npages, size;
	unsigned long chunk_size = conf->chunk_size;

	mbio = rxbio->master_bio;

	printk(KERN_INFO "raidxor: splitting from sector %llu, %llu bytes\n",
	       (unsigned long long) mbio->bi_sector,
	       (unsigned long long) mbio->bi_size);

	/* reading at most one sector more then necessary on (each disk - 1) */
	size = chunk_size *
		((mbio->bi_size / chunk_size) /  +
		 ((mbio->bi_size / chunk_size) % conf->n_resources) ? 1 : 0);
	npages = size / PAGE_SIZE + (size % PAGE_SIZE) ? 1 : 0;
	printk(KERN_INFO "raidxor: splitting into requests of size %llu a %lu pages\n",
	       (unsigned long long) size, npages);

	for (i = 0; i < conf->n_resources; ++i) {
		rbio = bio_alloc(GFP_NOIO, npages);
		if (!rbio)
			goto out_free_pages;
		rxbio->bios[i] = rbio;

		rbio->bi_rw = READ;
		rbio->bi_private = rxbio;

		rbio->bi_bdev = conf->units[i].rdev->bdev;
		rbio->bi_sector = rxbio->master_bio->bi_sector / conf->n_resources +
			conf->units[i].rdev->data_offset;
		rbio->bi_size = size;

		printk(KERN_INFO "raidxor: request %lu goes to physical sector %llu\n",
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

	return 0;
out_free_pages:
	for (i = 0; i < conf->n_resources; ++i)
		if (rxbio->bios[i]) {
			for (j = 0; j < npages; ++j)
				safe_put_page(rxbio->bios[i]->bi_io_vec[j].bv_page);
			bio_put(rxbio->bios[i]);
		}
	return 1;
}

static void raidxord(mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	int unplug = 0;
	raidxor_bio_t *rxbio;
	unsigned long i, handled = 0;

	printk(KERN_INFO "raidxor: raidxord active\n");

	spin_lock(&conf->device_lock);
	for (;;) {
		if (list_empty(&conf->handle_list)) {
			break;
		}
		rxbio = list_entry(conf->handle_list.next, typeof(*rxbio), lru);
		list_del_init(&rxbio->lru);

		spin_unlock(&conf->device_lock);
		for (i = 0; i < rxbio->n_bios; ++i)
			generic_make_request(rxbio->bios[i]);
		++handled;
		spin_lock(&conf->device_lock);
	}
	spin_unlock(&conf->device_lock);

	printk(KERN_INFO "raidxor: raidxord inactive, handled %lu requests\n");
}

static int raidxor_run(mddev_t *mddev)
{
	raidxor_conf_t *conf;
	struct list_head *tmp;
	mdk_rdev_t* rdev;
	char buffer[32];
	sector_t size;
	unsigned long i;

	if (mddev->level != LEVEL_XOR) {
		printk(KERN_ERR "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}

	if (mddev->chunk_size < PAGE_SIZE) {
		printk(KERN_ERR "raidxor: chunk_size must be at least "
		       "PAGE_SIZE but %d < %ld\n",
		       mddev->chunk_size, PAGE_SIZE);
		goto out_inval;
	}

	printk(KERN_INFO "raidxor: raid set %s active with %d disks\n",
	       mdname(mddev), mddev->raid_disks);

	if (mddev->raid_disks < 1)
		goto out_inval;

	conf = kzalloc(sizeof(raidxor_conf_t) +
		       sizeof(struct disk_info) * mddev->raid_disks, GFP_KERNEL);
	mddev->private = conf;
	if (!conf) {
		printk(KERN_ERR "raidxor: couldn't allocate memory for %s\n",
		       mdname(mddev));
		goto out;
	}

	conf->configured = 0;
	conf->mddev = mddev;
	conf->units_per_resource = 0;
	conf->n_resources = 0;
	conf->resources = NULL;
	conf->n_stripes = 0;
	conf->stripes = NULL;
	conf->n_units = mddev->raid_disks;

	spin_lock_init(&conf->device_lock);
	mddev->queue->queue_lock = &conf->device_lock;
	blk_queue_hardsect_size(mddev->queue, PAGE_SIZE);

	INIT_LIST_HEAD(&conf->handle_list);

	size = -1; /* in sectors, that is 1024 byte */

	i = conf->n_units - 1;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		//index = rdev->raid_disk;

		printk(KERN_INFO "raidxor: device %lu rdev %s, %llu blocks\n",
		       i, bdevname(rdev->bdev, buffer),
		       (unsigned long long) rdev->size * 2);
		conf->units[i].rdev = rdev;
		conf->units[i].redundant = -1;

		--i;
	}
	if (size == -1)
		goto out_free_conf;

	/* FIXME: used component size, multiple of chunk_size ... */
	mddev->size = size; // & ~(mddev->chunk_size / 1024 - 1); 
	/* exported size */
	mddev->array_sectors = 0;

	/* Ok, everything is just fine now */
	if (sysfs_create_group(&mddev->kobj, &raidxor_attrs_group)) {
		printk(KERN_ERR
		       "raidxor: failed to create sysfs attributes for %s\n",
		       mdname(mddev));
		goto out_free_conf;
	}

	mddev->thread = md_register_thread(raidxord, mddev, "%s_raidxor");
	if (!mddev->thread) {
		printk(KERN_ERR
		       "raidxor: couldn't allocate thread for %s\n",
		       mdname(mddev));
		goto out_free_sysfs;
	}

	return 0;

out_free_sysfs:
	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);

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

	md_unregister_thread(mddev->thread);
	mddev->thread = NULL;

	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);

	mddev_to_conf(mddev) = NULL;
	raidxor_safe_free_conf(conf);
	kfree(conf);

	return 0;
}

static void raidxor_status(struct seq_file *seq, mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	seq_printf(seq, " I'm feeling fine");
	return;
}

static stripe_t * raidxor_sector_to_stripe(raidxor_conf_t *conf, sector_t sector)
{
	int sectors_per_chunk = conf->chunk_size >> 9;
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio) {
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	const int rw = bio_data_dir(bio);
	raidxor_bio_t *rxbio;
	int i, j;

	printk(KERN_INFO "raidxor: got request\n");

	goto out;

	/* we don't handle read requests yet */
	/* apparently, we do have to handle them ... */
	if (rw == READ) {
		printk (KERN_INFO "raidxor: handling read request\n");

		/* TODO: create a pool for this */
		//rxbio = mempool_alloc(conf->rxbio_pool, GFP_NOIO);

		rxbio = kzalloc(sizeof(raidxor_bio_t) +
				sizeof(struct bio *) * conf->n_resources, GFP_NOIO);
		if (!rxbio)
			goto out_free_rxbio;

		rxbio->master_bio = bio;
		rxbio->mddev = mddev;
		rxbio->stripe = raidxor_sector_to_stripe(conf, bio->bi_sector);
		rxbio->n_bios = conf->n_resources;
		atomic_set(&rxbio->remaining, rxbio->n_bios);

		list_add_tail(&rxbio->lru, &conf->handle_list);
		md_wakeup_thread(conf->mddev->thread);

		return 0;
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
