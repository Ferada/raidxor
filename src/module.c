#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/div64.h>

#include "raidxor.h"

#include "utils.c"
#include "conf.c"

#define RAIDXOR_RUN_TESTCASES 1

static int raidxor_cache_make_clean(cache_t *cache, unsigned int line)
{
	if (!cache || line >= cache->n_lines) goto out;
	if (cache->lines[line].flags == CACHE_LINE_CLEAN) goto out_success;
	if (cache->lines[line].flags != CACHE_LINE_READY) goto out;

	cache->lines[line].flags = CACHE_LINE_CLEAN;
	raidxor_cache_drop_line(cache, line);

out_success:
	return 0;
out:
	return 1;
}

static int raidxor_cache_make_ready(cache_t *cache, unsigned int line)
{
	unsigned int i;

	CHECK(cache, out, "no cache");
	CHECK(line < cache->n_lines, out, "n not inside number of lines");

	if (cache->lines[line].flags == CACHE_LINE_READY) goto out_success;
	if (cache->lines[line].flags != CACHE_LINE_CLEAN) goto out;

	cache->lines[line].flags = CACHE_LINE_READY;

	for (i = 0; i < cache->n_buffers; ++i)
		if (!(cache->lines[line].buffers[i] = alloc_page(GFP_NOIO)))
			goto out_free_pages;

out_success:
	return 0;
out_free_pages:
	raidxor_cache_make_clean(cache, line);
out:
	return 1;
}

static void raidxor_end_load_line(struct bio *bio, int error);
static void raidxor_end_writeback_line(struct bio *bio, int error);

static int raidxor_cache_load_line(cache_t *cache, unsigned int n)
{
	cache_line_t *line;
	stripe_t *stripe;
	sector_t actual_sector;
	raidxor_bio_t *rxbio;
	struct bio *bio;
	unsigned int i, j, k, n_chunk_mult;
	raidxor_conf_t *conf = cache->conf;

	CHECK(cache, out, "no cache");
	CHECK(n < cache->n_lines, out, "n not inside number of lines");

	line = &cache->lines[n];
	CHECK(line->flags == CACHE_LINE_READY, out, "line not in ready state");

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK(stripe, out, "no stripe found");
	
	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK(rxbio, out, "couldn't allocate raidxor_bio_t");

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0; i < rxbio->n_bios; ++i) {
		if (stripe->units[i]->redundant)
			continue;
		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, cache->n_chunk_mult);
		CHECK(rxbio->bios[i], out_free_bio,
		      "couldn't allocate bio");

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags)) {
			printk(KERN_INFO "raidxor: got a faulty drive"
			       " during a request, skipping for now");
			goto out_free_bio;
		}

		bio->bi_rw = READ;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_load_line;
		bio->bi_sector = 42;
		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		/* assign page */
		bio->bi_vcnt = n_chunk_mult;
		for (j = 0; j < n_chunk_mult; ++j) {
			k = i * n_chunk_mult + j;
			bio->bi_io_vec[k].bv_page = line->buffers[k];
			bio->bi_io_vec[k].bv_len = PAGE_SIZE;
			bio->bi_io_vec[k].bv_offset = 0;
		}
	}

	rxbio->remaining = stripe->n_data_units;
	++cache->active_lines;
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i])
			generic_make_request(rxbio->bios[i]);

	return 0;
out_free_bio:
	raidxor_free_bio(rxbio);
out:
	/* bio_error the listed requests */
	return 1;
}

static int raidxor_cache_writeback_line(cache_t *cache, unsigned int n)
{
	cache_line_t *line;
	stripe_t *stripe;
	sector_t actual_sector;
	raidxor_bio_t *rxbio;
	unsigned int i, j, k, n_chunk_mult;
	struct bio *bio;
	raidxor_conf_t *conf = cache->conf;

	CHECK(cache, out, "no cache");
	CHECK(n < cache->n_lines, out, "n not inside number of lines");

	line = &cache->lines[n];
	CHECK(line->flags == CACHE_LINE_DIRTY, out, "line not in dirty state");

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK(stripe, out, "no stripe found");

	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK(rxbio, out, "couldn't allocate raidxor_bio_t");

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0; i < rxbio->n_bios; ++i) {
		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, cache->n_chunk_mult);
		CHECK(rxbio->bios[i], out_free_bio,
		      "couldn't allocate bio");

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags)) {
			printk(KERN_INFO "raidxor: got a faulty drive"
			       " during a request, skipping for now");
			goto out_free_bio;
		}

		bio->bi_rw = WRITE;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_writeback_line;
		bio->bi_sector = 42;
		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		/* assign page */
		bio->bi_vcnt = n_chunk_mult;
		for (j = 0; j < n_chunk_mult; ++j) {
			k = i * n_chunk_mult + j;
			bio->bi_io_vec[k].bv_page = line->buffers[k];
			bio->bi_io_vec[k].bv_len = PAGE_SIZE;
			bio->bi_io_vec[k].bv_offset = 0;
		}		
	}

	for (i = 0; i < rxbio->n_bios; ++i)
		if (stripe->units[i]->redundant) {
			if (raidxor_xor_combine(rxbio->bios[i], rxbio, stripe->units[i]->encoding))
				goto out_free_bio;
		}

	rxbio->remaining = rxbio->n_bios;
	++cache->active_lines;
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i])
			generic_make_request(rxbio->bios[i]);

	return 0;
out_free_bio:
	raidxor_free_bio(rxbio);
out:
	return 1;
}

static void raidxor_end_load_line(struct bio *bio, int error)
{
	raidxor_bio_t *rxbio = (raidxor_bio_t *)(bio->bi_private);

	if (error) {
		/* TODO: set faulty bit on that device */
	}
	else {
		/* TODO: copy data around */
	}

	WITHLOCKCONF(rxbio->cache->conf, {
	if ((--rxbio->remaining) == 0) {
		if (rxbio->line->flags == CACHE_LINE_LOADING) {
			rxbio->line->flags = CACHE_LINE_UPTODATE;
		}
		--rxbio->cache->active_lines;
		/* TODO: wake up waiting threads */

		kfree(rxbio);
	}
	});
}

static void raidxor_end_writeback_line(struct bio *bio, int error)
{
	raidxor_bio_t *rxbio = (raidxor_bio_t *)(bio->bi_private);

	if (error) {
		/* TODO: set faulty bit on that device */
		WITHLOCKCONF(rxbio->cache->conf, {
			rxbio->line->flags = CACHE_LINE_DIRTY;
		});
	}
	else {
		/* TODO: copy data around */
	}

	WITHLOCKCONF(rxbio->cache->conf, {
	if ((--rxbio->remaining) == 0) {
		if (rxbio->line->flags == CACHE_LINE_WRITEBACK) {
			rxbio->line->flags = CACHE_LINE_UPTODATE;
		}
		--rxbio->cache->active_lines;
		/* TODO: wake up waiting threads */

		kfree(rxbio);
	}
	});
}

/**
 * raidxor_compute_length() - computes the number of bytes to be read
 *
 * That is, given the position and length in the virtual device, how
 * many blocks do we actually have to copy from a single request.
 */
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

/**
 * raidxor_xor_single() - xors the buffers of two bios together
 *
 * Both bios have to be of the same size and layout.
 */
static void raidxor_xor_single(struct bio *bioto, struct bio *biofrom)
{
	unsigned long i, j;
	struct bio_vec *bvto, *bvfrom;
	unsigned char *tomapped, *frommapped;
	unsigned char *toptr, *fromptr;

	for (i = 0; i < bioto->bi_vcnt; ++i) {
		bvto = bio_iovec_idx(bioto, i);
		bvfrom = bio_iovec_idx(biofrom, i);

		tomapped = (unsigned char *) kmap(bvto->bv_page);
		frommapped = (unsigned char *) kmap(bvfrom->bv_page);

		toptr = tomapped + bvto->bv_offset;
		fromptr = frommapped + bvfrom->bv_offset;

		for (j = 0; j < bvto->bv_len; ++j, ++toptr, ++fromptr) {
			*toptr ^= *fromptr;
		}

		kunmap(bvfrom->bv_page);
		kunmap(bvto->bv_page);
	}
}

/**
 * raidxor_check_same_size_and_layout() - checks two bios
 *
 * Returns 0 if both bios have the same size and are layed out the same way, else 1.
 */
static int raidxor_check_same_size_and_layout(struct bio *x, struct bio *y)
{
	unsigned long i;

	/* two bios are the same, if they are of the same size, */
	if (x->bi_size != y->bi_size)
		return 1;

	/* have the same number of bio_vecs, */
	if (x->bi_vcnt != y->bi_vcnt)
		return 2;

	/* and those are of the same length, pairwise */
	for (i = 0; i < x->bi_vcnt; ++i) {
		/* FIXME: if this not printd, the test fails */
		printk(KERN_INFO "comparing %d and %d\n",
		   x->bi_io_vec[i].bv_len, y->bi_io_vec[i].bv_len);
		if (x->bi_io_vec[i].bv_len != y->bi_io_vec[i].bv_len)
			return 3;
	}

	return 0;
}

/**
 * raidxor_xor_combine() - xors a number of resources together
 *
 * Takes a master request and combines the request inside the rxbio together
 * using the given encoding for the unit.
 *
 * Returns 1 on error (bioto still might be touched in this case).
 */
static int raidxor_xor_combine(struct bio *bioto, raidxor_bio_t *rxbio,
			       encoding_t *encoding)
{
	/* since we have control over bioto and rxbio, every bio has size
	   M * CHUNK_SIZE with CHUNK_SIZE = N * PAGE_SIZE */
	unsigned long i;
	struct bio *biofrom;

	CHECK(bioto, out, "bioto missing");
	CHECK(rxbio, out, "bioto missing");
	CHECK(encoding, out, "bioto missing");

	/* copying first bio buffers */
	biofrom = raidxor_find_bio(rxbio, encoding->units[0]);
	raidxor_copy_bio(bioto, biofrom);

	/* then, xor the other buffers to the first one */
	for (i = 1; i < encoding->n_units; ++i) {
		printk(KERN_INFO "encoding unit %lu out of %d\n", i,
		       encoding->n_units);
		/* search for the right bio */
		biofrom = raidxor_find_bio(rxbio, encoding->units[i]);

		if (!biofrom) {
			printk(KERN_DEBUG "raidxor: didn't find bio in"
			       " raidxor_xor_combine\n");
			goto out;
		}

		if (raidxor_check_same_size_and_layout(bioto, biofrom)) {
			printk(KERN_DEBUG "raidxor: bioto and biofrom"
			       " differ in size and/or layout\n");
			goto out;
		}

		printk(KERN_INFO "combining %p and %p\n", bioto, biofrom);

		/* combine the data */
		raidxor_xor_single(bioto, biofrom);
	}

	return 0;
out:
	return 1;
}

static void raidxor_error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	char buffer[BDEVNAME_SIZE];
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	disk_info_t *unit = raidxor_find_unit_conf_rdev(conf, rdev);

	WITHLOCKCONF(conf, {
	if (!test_bit(Faulty, &rdev->flags)) {
		/* escalate error */
		set_bit(Faulty, &rdev->flags);
		set_bit(STRIPE_FAULTY, &unit->stripe->flags);
		set_bit(CONF_FAULTY, &conf->flags);
		printk(KERN_ALERT "raidxor: disk failure on %s, disabling"
		       " device\n", bdevname(rdev->bdev, buffer));
	}
	});
}

static void raidxor_compute_length_and_pages(stripe_t *stripe,
					     struct bio *mbio,
					     unsigned long *length,
					     unsigned long *npages)
{
	/* mbio->bi_size = M * chunk_size * n_data_units
	   ---------------------------------------------  = M * chunk_size
	   n_data_units
	*/
	unsigned long tmp = mbio->bi_size / stripe->n_data_units;

	/* number of bytes to write to each data unit */
	*length = tmp;
	*npages = tmp / PAGE_SIZE;
}

/**
 * raidxord() - daemon thread
 *
 * Is started by the md level.  Takes requests from the queue and handles them.
 */
static void raidxord(mddev_t *mddev)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long handled = 0;

	/* someone poked us.  see what we can do */
	printk(KERN_INFO "raidxor: raidxord active\n");

	WITHLOCKCONF(conf, {
	for (; !test_bit(CONF_STOPPING, &conf->flags);) {
		/* go through all cache lines, see if anything can be done */
		/* if the target device is faulty, start repair if possible,
		   else signal an error on that request */
		UNLOCKCONF(conf);
		LOCKCONF(conf);
	}
	});

	printk(KERN_INFO "raidxor: raidxord inactive, handled %lu requests\n",
	       handled);
}

/**
 * raidxor_run() - basic initialization for the raid
 *
 * We can't use it after this, because the layout of the raid is not
 * described yet.  Therefore, every read/write operation fails until
 * we've got enough information.
 */
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

	printk(KERN_EMERG "whoo, setting hardsect size to %d\n", 4096);
	blk_queue_hardsect_size(mddev->queue, 4096);

	spin_lock_init(&conf->device_lock);
	mddev->queue->queue_lock = &conf->device_lock;

	init_waitqueue_head(&conf->wait_for_line);

	size = -1; /* rdev->size is in sectors, that is 1024 byte */

	i = conf->n_units - 1;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		printk(KERN_INFO "raidxor: device %lu rdev %s, %llu blocks\n",
		       i, bdevname(rdev->bdev, buffer),
		       (unsigned long long) rdev->size * 2);
		conf->units[i].rdev = rdev;
		conf->units[i].redundant = -1;

		--i;
	}
	if (size == -1)
		goto out_free_conf;

	printk(KERN_INFO "raidxor: used component size: %llu sectors\n",
	       (unsigned long long) size & ~(conf->chunk_size / 1024 - 1));

	/* used component size in sectors, multiple of chunk_size ... */
	mddev->size = size & ~(conf->chunk_size / 1024 - 1);
	/* exported size, will be initialised later */
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

	WITHLOCKCONF(conf, {
	set_bit(CONF_STOPPING, &conf->flags);
	});

#if 0
	wait_event_lock_irq(conf->wait_for_line,
			    atomic_read(&conf->cache->active_lines) == 0,
			    conf->device_lock, /* nothing */);
#endif

	md_unregister_thread(mddev->thread);
	mddev->thread = NULL;

	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);

	mddev_to_conf(mddev) = NULL;
	raidxor_safe_free_conf(conf);
	kfree(conf);

	return 0;
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio)
{
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	const int rw = bio_data_dir(bio);
	stripe_t *stripe;
	sector_t newsector;
	struct bio_pair *split;

	printk(KERN_EMERG "raidxor: got request\n");

	printk(KERN_EMERG "raidxor: sector_to_stripe(conf, %llu, &newsector) called\n",
	       (unsigned long long) bio->bi_sector);

	WITHLOCKCONF(conf, {
	stripe = raidxor_sector_to_stripe(conf, bio->bi_sector, &newsector);
	CHECK(stripe, out_unlock, "no stripe found");

#if 0
	if (raidxor_bio_maybe_split_boundary(stripe, bio, newsector, &split)) {
		if (!split)
			goto out_unlock;

		generic_make_request(&split->bio1);
		generic_make_request(&split->bio2);
		bio_pair_release(split);

		return 0;
	}
#endif

	/* if no line is available, wait for it ... */
	/* pack the request somewhere in the cache */
	});

	md_wakeup_thread(conf->mddev->thread);

	printk(KERN_INFO "raidxor: handling %s request\n",
	       (rw == READ) ? "read" : "write");

	return 0;

out_unlock:
	UNLOCKCONF(conf);
/* out: */
	bio_io_error(bio);
	return 0;
}

#include "init.c"
