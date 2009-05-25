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
	unsigned long i;

	if (!conf) {
		printk(KERN_DEBUG "raidxor: NULL pointer "
		       "in raidxor_safe_free_conf\n");
		return;
	}

	if (conf->resources != NULL) {
		for (i = 0; i < conf->n_resources; ++i)
			kfree(conf->resources[i]);
		kfree(conf->resources);
		conf->resources = NULL;
	}

	if (conf->stripes != NULL) {
		for (i = 0; i < conf->n_stripes; ++i)
			kfree(conf->stripes[i]);
		kfree(conf->stripes);
		conf->stripes = NULL;
	}
	
	conf->configured = 0;
}

static void raidxor_try_configure_raid(raidxor_conf_t *conf) {
	raidxor_resource_t **resources;
	stripe_t **stripes;
	disk_info_t *unit;
	unsigned long i, j;
	char buffer[32];
	mddev_t *mddev = conf->mddev;
	sector_t size = 0;

	if (!conf) {
		printk(KERN_DEBUG "raidxor: NULL pointer in "
		       "raidxor_free_conf\n");
		return;
	}

	if (conf->n_resources <= 0 || conf->units_per_resource <= 0) {
		printk(KERN_INFO "raidxor: need number of resources or "
		       "units per resource: %lu or %lu\n",
		       conf->n_resources, conf->units_per_resource);
		goto out;
	}

	if (conf->n_resources * conf->units_per_resource != conf->n_units) {
		printk(KERN_INFO
		       "raidxor: parameters don't match %lu * %lu != %lu\n",
		       conf->n_resources, conf->units_per_resource,
		       conf->n_units);
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

	resources = kzalloc(sizeof(raidxor_resource_t *) * conf->n_resources,
			    GFP_KERNEL);
	if (!resources)
		goto out;

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i] = kzalloc(sizeof(raidxor_resource_t) +
				       (sizeof(disk_info_t *) *
					conf->units_per_resource), GFP_KERNEL);
		if (!resources[i])
			goto out_free_res;
	}

	conf->n_stripes = conf->units_per_resource;

	stripes = kzalloc(sizeof(stripe_t *) * conf->n_stripes, GFP_KERNEL);
	if (!stripes)
		goto out_free_res;

	for (i = 0; i < conf->n_stripes; ++i) {
		stripes[i] = kzalloc(sizeof(stripe_t) +
				     (sizeof(disk_info_t *) *
				      conf->n_resources), GFP_KERNEL);
		if (!stripes[i])
			goto out_free_stripes;
	}

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i]->n_units = conf->units_per_resource;
		for (j = 0; j < conf->units_per_resource; ++j) {
			unit = &conf->units[i + j * conf->n_resources];

			unit->resource = resources[i];
			resources[i]->units[j] = unit;
		}
	}

	printk(KERN_INFO "now calculating stripes and sizes\n");

	for (i = 0; i < conf->n_stripes; ++i) {
		printk(KERN_INFO "direct: stripes[%lu] %p\n", i, stripes[i]);
		stripes[i]->n_units = conf->n_resources;
		stripes[i]->size = 0;

		for (j = 0; j < stripes[i]->n_units; ++j) {
			unit = &conf->units[i * stripes[i]->n_units + j];

			unit->stripe = stripes[i];
			if (unit->redundant == 0) {
				++stripes[i]->n_data_units;
				stripes[i]->size += (unit->rdev->size * 2) &
					~(conf->chunk_size / 512 - 1);
			}
			stripes[i]->units[j] = unit;
		}
		size += stripes[i]->size / 2;
	}

	printk(KERN_INFO "setting device size\n");

	if (!mddev)
		goto out_free_stripes;

	/* FIXME: device size must a multiple of chunk size */
	mddev->array_sectors = size;
	set_capacity(mddev->gendisk, mddev->array_sectors);

	printk (KERN_INFO "raidxor: array_sectors is %llu blocks\n",
		(unsigned long long) mddev->array_sectors * 2);

	conf->resources = resources;
	conf->stripes = stripes;
	conf->configured = 1;

	return;

out_free_stripes:
	for (i = 0; i < conf->n_stripes; ++i)
		kfree(stripes[i]);
	kfree(stripes);
out_free_res:
	for (i = 0; i < conf->n_resources; ++i)
		kfree(resources[i]);
	kfree(resources);
out:
	return;
}

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

/*
  everything < 4096 Bytes !

  /sys/md/raidxor/number_of_resources:
  [number_of_resources]

  /sys/md/raidxor/units_per_resource
  [number_of_units_per_resource]

  then we have a grid and can assign the units

  /sys/md/raidxor/redundancy:
  [index][redundant][length_of_equation]
    [index] ... [index]
  [index][not_redundant]
  ...
 */
static ssize_t
raidxor_store_encoding(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned char index, redundant, length, i, red;
	encoding_t *encoding;
	size_t oldlen = len;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	for (; len >= 2;) {
		index = *page++;
		--len;

		if (index >= conf->n_units)
			goto out;

		redundant = *page++;
		--len;

		if (redundant != 0 && redundant != 1)
			return -EINVAL;

		conf->units[index].redundant = redundant;

		if (redundant == 0) {
			printk(KERN_INFO "read non-redundant unit info\n");
			continue;
		}

		if (len < 1)
			goto out_reset;

		length = *page++;
		--len;

		if (length > len)
			goto out_reset;

		encoding = kzalloc(sizeof(encoding_t) +
				   sizeof(disk_info_t *) * length, GFP_NOIO);
		if (!encoding)
			goto out_reset;

		for (i = 0; i < length; ++i) {
			red = *page++;
			--len;

			if (red >= conf->n_units)
				goto out_free_encoding;

			encoding->units[i] = &conf->units[red];
		}

		conf->units[index].encoding = encoding;

		printk(KERN_INFO "read redundant unit encoding info\n");
	}

	return oldlen;
out_free_encoding:
	kfree(encoding);
out_reset:
	conf->units[index].redundant = -1;
out:
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

/**
 * Copies LENGTH bytes from BIOFROM to BIOTO.  Writing starts at OFFSET from
 * the beginning of BIOTO and is performed at chunks of CHUNK_SIZE.  Data is
 * written at every first block of RASTER chunks.
 */
static void raidxor_scatter_copy_data(struct bio *bioto, struct bio *biofrom,
				      unsigned long length, unsigned long to_offset,
				      unsigned long chunk_size, unsigned int raster)
{
	unsigned int i = 0, j = 0;
	struct bio_vec *bvfrom = bio_iovec_idx(biofrom, i);
	struct bio_vec *bvto = bio_iovec_idx(bioto, j);
	char *mapped;
	unsigned int clen = 0;
	unsigned long from_offset = 0;
	unsigned long to_copy = min(length, chunk_size);

#define NEXT_BVFROM do { bvfrom = bio_iovec_idx(biofrom, ++i); } while (0)
#define NEXT_BVTO do { bvto = bio_iovec_idx(bioto, ++j); } while (0)

	/* adjust offset and the vector so we actually write inside it */
	while (to_offset >= bvto->bv_len) {
		to_offset -= bvto->bv_len;
		NEXT_BVTO;
	}

	mapped = __bio_kmap_atomic(bioto, j, KM_USER0);
	while (length > 0) {
		clen = (to_copy > bvfrom->bv_len) ? to_copy : bvfrom->bv_len;
		clen = (clen > bvto->bv_len) ? clen : bvto->bv_len;
		//clen = min(min(to_copy, bvfrom->bv_len), bvto->bv_len);

		memcpy(mapped + bvto->bv_offset + to_offset,
		       bvfrom->bv_page + bvfrom->bv_offset + from_offset,
		       clen);

		to_copy -= clen;
		length -= clen;
		from_offset += clen;
		to_offset += clen;

		/* our chunk is not finished yet, so we adjust our pointers */
		if (to_copy > 0) {
			if (to_offset == bvto->bv_len) {
				to_offset = 0;
				NEXT_BVTO;
			}
		}
		/* our chunk is finished, but we're still not done */
		else if (length > 0) {
			to_offset += (raster - 1) * chunk_size;

			while (to_offset >= bvto->bv_len) {
				to_offset -= bvto->bv_len;
				NEXT_BVTO;
			}
		}
		if (from_offset == bvfrom->bv_len) {
			from_offset = 0;
			NEXT_BVFROM;
		}
	}
#undef NEXT_BVFROM
#undef NEXT_BVTO

	__bio_kunmap_atomic(bioto, KM_USER0);
}

static unsigned long raidxor_compute_length(raidxor_conf_t *conf,
					    stripe_t *stripe,
					    struct bio *mbio,
					    unsigned long index)
{
	/* copy this for every unit in the stripe */
	unsigned long result = (mbio->bi_size / (conf->chunk_size * stripe->n_data_units))
		* conf->chunk_size;

	unsigned long over = mbio->bi_size % (conf->chunk_size *
					      stripe->n_data_units);

	/* now, this is under n_data_units * chunk_size, so lets deal with it,
	   if we have more than a full strip, copy those bytes from the
	   remaining units, but really only what we need */
	if (over > index * conf->chunk_size)
		result += min(conf->chunk_size, over - index * conf->chunk_size);

	return result;
}

static void raidxor_end_read_request(struct bio *bio, int error)
{
	raidxor_bio_t *rxbio = (raidxor_bio_t *)(bio->bi_private);
	raidxor_conf_t *conf = mddev_to_conf(rxbio->mddev);
	unsigned long i, j, index;
	struct bio *mbio = rxbio->master_bio;
	struct bio_vec *bvfrom, *bvto;
	unsigned long from_offset, to_offset;
	char *mapped;
	stripe_t *stripe = rxbio->stripe;
	unsigned long length;

	printk(KERN_INFO "raidxor_end_read_request\n");

	goto out;

	for (i = 0, index = 0; i < stripe->n_units; ++i) {
		if (stripe->units[i]->redundant == 0)
			++index;
		if (stripe->units[i]->rdev->bdev == bio->bi_bdev)
			break;
	}

	/* offset for the copy operations */
	to_offset = index * conf->chunk_size;

	/* if we don't have something to copy, do nothing.
	   even better: move this check to make_request :) */
	if (to_offset >= bio->bi_size)
		goto out;

	length = raidxor_compute_length(conf, stripe, mbio, index);

	printk(KERN_INFO "raidxor_end_read_request: %lu will copy %lu bytes,"
	       " or %lu blocks to mbio\n", index, length, length >> 9);

	raidxor_scatter_copy_data(mbio, bio, length, to_offset,
				  conf->chunk_size, stripe->n_data_units);

	for (i = 0; i < bio->bi_vcnt; ++i)
		safe_put_page(bio->bi_io_vec[i].bv_page);
	bio_put(bio);

	printk(KERN_INFO "put page, remaining %lu\n", rxbio->remaining);

	if (rxbio->remaining == 0) {
		bio_endio(mbio, 0);
		kfree(rxbio);
	}
	else {
		--rxbio->remaining;
	}
}

static void raidxor_free_bios(raidxor_bio_t *rxbio)
{
	unsigned long i, j;
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]) {
			for (j = 0; j < rxbio->bios[i]->bi_vcnt; ++j)
				safe_put_page(rxbio->bios[i]->bi_io_vec[j].bv_page);
			bio_put(rxbio->bios[i]);
		}
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
	unsigned long i, j, k;
	struct bio *mbio, *rbio;
	struct page *page;
	unsigned long npages, size;
	unsigned long chunk_size = conf->chunk_size;
	stripe_t *stripe = rxbio->stripe;
	disk_info_t *unit;

	mbio = rxbio->master_bio;

	printk(KERN_INFO "raidxor: splitting from sector %llu, %llu bytes\n",
	       (unsigned long long) mbio->bi_sector,
	       (unsigned long long) mbio->bi_size);

	rxbio->n_bios = stripe->n_data_units;

	/* reading at most one sector more then necessary on (each disk - 1) */
	size = chunk_size *
		((mbio->bi_size / chunk_size) / stripe->n_data_units +
		 ((mbio->bi_size / chunk_size) % stripe->n_data_units) ? 1 : 0);
	npages = size / PAGE_SIZE + (size % PAGE_SIZE) ? 1 : 0;
	printk(KERN_INFO "raidxor: splitting into requests of size %llu a %lu pages\n",
	       (unsigned long long) size, npages);

	printk(KERN_INFO "%lu bios\n", rxbio->n_bios);

	for (i = 0, k = 0; i < rxbio->n_bios; ++i) {
		printk(KERN_INFO "loop %lu\n", i);

		rbio = bio_alloc(GFP_NOIO, npages);
		if (!rbio) {
			printk(KERN_INFO "couldn't allocate bio\n");
			goto out_free_pages;
		}
		rxbio->bios[i] = rbio;

		rbio->bi_rw = READ;
		rbio->bi_private = rxbio;

		/* get the right data units */
		for (;;) {
			printk(KERN_INFO "%lu + %lu = %lu\n",
				i, k, i + k);

			if (i + k >= stripe->n_units ||
				stripe->units[i + k]->redundant == 0)
				break;

			++k;
		}

		if (k == stripe->n_units) {
			printk(KERN_INFO "couldn't find device, %lu\n", k);
			goto out_free_pages;
		}
		rbio->bi_bdev = stripe->units[i + k]->rdev->bdev;

		printk(KERN_INFO "raidxor: device is %lu\n", i + k);

		rbio->bi_sector = stripe->units[i + k]->rdev->data_offset +
			rxbio->sector / (chunk_size >> 9);
		//rbio->bi_sector = rxbio->master_bio->bi_sector / conf->n_resources +
		//	conf->units[i].rdev->data_offset;
		rbio->bi_size = size;
		rbio->bi_vcnt = npages;

		printk(KERN_INFO "raidxor: sector %lu, chunk_size >> 9 = %lu, data_offset %lu\n",
		       rxbio->sector,
		       chunk_size >> 9,
		       stripe->units[i + k]->rdev->data_offset);

		printk(KERN_INFO "raidxor: request %lu goes to physical sector %llu\n",
		       i, (unsigned long long) rbio->bi_sector);

		rbio->bi_end_io = raidxor_end_read_request;

		for (j = 0; j < npages; ++j) {
			page = alloc_page(GFP_NOIO);
			if (!page) {
				printk(KERN_INFO "raidxor: couldn't allocate pages\n");
				goto out_free_pages;
			}

			rbio->bi_io_vec[j].bv_page = page;
			rbio->bi_io_vec[j].bv_len = PAGE_SIZE;
			rbio->bi_io_vec[j].bv_offset = j * PAGE_SIZE;
		}
	}

	return 0;

out_free_pages:
	raidxor_free_bios(rxbio);
out_free_rxbio:
	kfree(rxbio);
	bio_io_error(mbio);
	return 1;
}

static void raidxord(mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	raidxor_bio_t *rxbio;
	unsigned long i, handled = 0;

	printk(KERN_INFO "raidxor: raidxord active\n");

	spin_lock(&conf->device_lock);
	for (;;) {
		if (list_empty(&conf->handle_list))
			break;

		rxbio = list_entry(conf->handle_list.next, typeof(*rxbio), lru);
		list_del_init(&rxbio->lru);

		if (raidxor_prepare_read_bio(conf, rxbio)) {
			printk(KERN_INFO "raidxor: unfinished request aborted\n");
			continue;
		}

		spin_unlock(&conf->device_lock);
		for (i = 0; i < rxbio->n_bios; ++i)
			generic_make_request(rxbio->bios[i]);
		++handled;
		spin_lock(&conf->device_lock);
	}
	spin_unlock(&conf->device_lock);

	printk(KERN_INFO "raidxor: raidxord inactive, handled %lu requests\n",
		handled);
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
	conf->chunk_size = mddev->chunk_size;
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

	/* used component size, multiple of chunk_size ... */
	mddev->size = size & ~(conf->chunk_size / 1024 - 1);
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

static int raidxor_bio_on_boundary(stripe_t *stripe, struct bio *bio,
				   sector_t newsector)
{
	return (stripe->size - (newsector << 9)) < bio->bi_size;
}

static stripe_t * raidxor_sector_to_stripe(raidxor_conf_t *conf, sector_t sector,
					   sector_t *newsector)
{
	unsigned int sectors_per_chunk = conf->chunk_size >> 9;
	stripe_t **stripes = conf->stripes;
	unsigned long i;

	printk(KERN_INFO "raidxor: sectors_per_chunk %u\n",
	       sectors_per_chunk);

	for (i = 0; i < conf->n_stripes; ++i) {
		printk(KERN_INFO "raidxor: stripe %lu, sector %lu\n", i, sector);
		if (sector <= stripes[i]->size >> 9)
			break;
		sector -= stripes[i]->size >> 9;
	}
	*newsector = sector;

	return stripes[i];
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio)
{
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	const int rw = bio_data_dir(bio);
	raidxor_bio_t *rxbio;
	stripe_t *stripe;
	sector_t newsector;

	printk(KERN_INFO "raidxor: got request\n");

	LOCKCONF(conf);

	stripe = raidxor_sector_to_stripe(conf, bio->bi_sector, &newsector);

	if (raidxor_bio_on_boundary(stripe, bio, newsector)) {
		printk(KERN_INFO "raidxor: FIXME: bio lies on boundary\n");
		goto out;
	}

	if (rw == READ) {
		printk (KERN_INFO "raidxor: handling read request\n");

		rxbio = kzalloc(sizeof(raidxor_bio_t) +
				sizeof(struct bio *) * conf->n_resources, GFP_NOIO);
		if (!rxbio)
			goto out;

		rxbio->master_bio = bio;
		rxbio->mddev = mddev;
		rxbio->stripe = stripe;
		rxbio->sector = newsector;
		rxbio->remaining = rxbio->n_bios;

		list_add_tail(&rxbio->lru, &conf->handle_list);
		md_wakeup_thread(conf->mddev->thread);

		UNLOCKCONF(conf);

		return 0;
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
	UNLOCKCONF(conf);
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
