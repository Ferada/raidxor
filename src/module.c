/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/div64.h>

#define DEBUG

#include "raidxor.h"

#include "utils.c"
#include "conf.c"

#define RAIDXOR_RUN_TESTCASES 1

static int raidxor_cache_make_clean(cache_t *cache, unsigned int line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	if (cache->lines[line].status == CACHE_LINE_CLEAN) return 0;
	CHECK_PLAIN_RET_VAL(cache->lines[line].status == CACHE_LINE_READY);

	cache->lines[line].status = CACHE_LINE_CLEAN;
	raidxor_cache_drop_line(cache, line);

	return 0;
}

static int raidxor_cache_make_ready(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	unsigned int i;
	cache_line_t *line;

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = &cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	if (line->status == CACHE_LINE_READY) return 0;
	CHECK_PLAIN_RET_VAL(line->status == CACHE_LINE_CLEAN || 
			    line->status == CACHE_LINE_UPTODATE);

	if (line->status == CACHE_LINE_CLEAN) {
		line->status = CACHE_LINE_READY;

		for (i = 0; i < cache->n_buffers; ++i)
			if (!(line->buffers[i] = alloc_page(GFP_NOIO)))
				goto out_free_pages;
	}

	line->status = CACHE_LINE_READY;

	return 0;
out_free_pages:
	raidxor_cache_make_clean(cache, n_line);
	return 1;
}

static int raidxor_cache_make_load_me(cache_t *cache, unsigned int line,
				      sector_t sector)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	if (cache->lines[line].status == CACHE_LINE_LOAD_ME) return 0;
	CHECK_PLAIN_RET_VAL(cache->lines[line].status == CACHE_LINE_READY);

	cache->lines[line].status = CACHE_LINE_LOAD_ME;
	cache->lines[line].sector = sector;

	return 0;
}

static void raidxor_cache_abort_line(cache_t *cache, unsigned int line)
{
	raidxor_cache_abort_requests(cache, line);
	cache->lines[line].status = CACHE_LINE_UPTODATE;
	raidxor_cache_make_ready(cache, line);
}

/**
 * raidxor_cache_recover() - tries to recover a cache line
 *
 * Since the read buffers are available, we can use them to calculate
 * the missing data.
 */
static void raidxor_cache_recover(cache_t *cache, unsigned int n_line)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	cache_line_t *line;
	raidxor_conf_t *conf;
	raidxor_bio_t *rxbio;

	CHECK_ARG(cache);
	CHECK_PLAIN(n_line < cache->n_lines);

	line = &cache->lines[n_line];
	CHECK_PLAIN(line);

	rxbio = line->rxbio;
	CHECK_PLAIN(rxbio);

	conf = cache->conf;
	CHECK_PLAIN(conf);

	if (1 /* TODO: we have don't have the equations */) {
		goto out;
	}

	line->status = CACHE_LINE_RECOVERY;

	/* TODO: do the recovery */

	line->status = CACHE_LINE_UPTODATE;
out:
	/* drop this line if an error occurs or we can't recover */
	raidxor_cache_abort_line(cache, n_line);
}

static unsigned int raidxor_bio_index(raidxor_bio_t *rxbio,
				      struct bio *bio,
				      unsigned int *data_index)
{
	unsigned int i, k;

	for (i = 0, k = 0; i < rxbio->n_bios; ++i) {
		if (rxbio->bios[i] == bio) {
			if (data_index)
				*data_index = k;
			return i;
		}
		if (!rxbio->stripe->units[i]->redundant)
			++k;
	}
	CHECK_BUG("didn't find bio");
	return 0;
}

static void raidxor_end_load_line(struct bio *bio, int error);
static void raidxor_end_writeback_line(struct bio *bio, int error);

static int raidxor_cache_load_line(cache_t *cache, unsigned int n)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	raidxor_conf_t *conf;
	cache_line_t *line;
	stripe_t *stripe;
	/* sector inside the stripe */
	sector_t actual_sector;
	raidxor_bio_t *rxbio;
	struct bio *bio;
	unsigned int i, j, k, n_chunk_mult;

	CHECK_ARG(cache);
	CHECK_PLAIN(n < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN(conf);

	line = &cache->lines[n];
	CHECK_PLAIN(line->status == CACHE_LINE_LOAD_ME);

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK_PLAIN(stripe);
	/* unrecoverable error, abort */
	CHECK_PLAIN(!test_bit(STRIPE_ERROR, &stripe->flags));

	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0; i < rxbio->n_bios; ++i) {
		/* we also load the redundant pages */

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags))
			continue;

		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		bio->bi_rw = READ;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_load_line;
		bio->bi_sector = actual_sector / stripe->n_data_units +
			stripe->units[i]->rdev->data_offset;
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

	line->rxbio = rxbio;
	rxbio->remaining = stripe->n_data_units;
	++cache->active_lines;
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i])
			generic_make_request(rxbio->bios[i]);

	return 0;
out_free_bio:
	raidxor_free_bio(rxbio);
out: __attribute__((unused))
	/* bio_error the listed requests */
	return 1;
}

static int raidxor_cache_writeback_line(cache_t *cache, unsigned int n)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	cache_line_t *line;
	stripe_t *stripe;
	sector_t actual_sector;
	raidxor_bio_t *rxbio;
	unsigned int i, j, k, n_chunk_mult;
	struct bio *bio;
	raidxor_conf_t *conf = cache->conf;

	CHECK_ARG(cache);
	CHECK_PLAIN(n < cache->n_lines);

	line = &cache->lines[n];
	CHECK_PLAIN(line->status == CACHE_LINE_DIRTY);

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK_PLAIN(stripe);

	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0; i < rxbio->n_bios; ++i) {
		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, cache->n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags)) {
			printk(KERN_INFO "raidxor: got a faulty drive"
			       " during a request, skipping for now");
			goto out_free_bio;
		}

		bio->bi_rw = WRITE;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_writeback_line;
		bio->bi_sector = actual_sector / stripe->n_data_units +
			stripe->units[i]->rdev->data_offset;
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

	for (i = 0, k = 0; i < rxbio->n_bios; ++i) {
		if (stripe->units[i]->redundant) continue;
		raidxor_copy_chunk_from_cache_line(conf, bio, line, k);
		++k;
	}

	for (i = 0; i < rxbio->n_bios; ++i) {
		if (!stripe->units[i]->redundant) continue;
		if (raidxor_xor_combine(rxbio->bios[i], rxbio,
					stripe->units[i]->encoding))
			goto out_free_bio;
	}

	line->rxbio = rxbio;
	rxbio->remaining = rxbio->n_bios;
	++cache->active_lines;
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i])
			generic_make_request(rxbio->bios[i]);

	return 0;
out_free_bio:
	raidxor_free_bio(rxbio);
out: __attribute__((unused))
	return 1;
}

static void raidxor_end_load_line(struct bio *bio, int error)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;
	cache_t *cache;
	cache_line_t *line;
	stripe_t *stripe;
	unsigned int index, data_index;

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	stripe = rxbio->stripe;

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = &cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	index = raidxor_bio_index(rxbio, bio, &data_index);

	if (error) {
		WITHLOCKCONF(conf, {
		line->status = CACHE_LINE_FAULTY;
		});
		md_error(conf->mddev, stripe->units[index]->rdev);
	}
	else if (!stripe->units[index]->redundant) {
		raidxor_copy_chunk_to_cache_line(conf, bio, line,
						 data_index);
	}

	WITHLOCKCONF(conf, {
	if ((--rxbio->remaining) == 0) {
		--rxbio->cache->active_lines;
		if (line->status == CACHE_LINE_LOADING) {
			line->status = CACHE_LINE_UPTODATE;
			kfree(rxbio);
			line->rxbio = NULL;
		}
		/* TODO: wake up waiting threads */
	}
	});
}

static void raidxor_end_writeback_line(struct bio *bio, int error)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	disk_info_t *unit;
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;
	cache_t *cache;
	cache_line_t *line;
	stripe_t *stripe;

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = &cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	stripe = rxbio->stripe;
	CHECK_PLAIN_RET(stripe);

	if (error) {
		WITHLOCKCONF(conf, {
		unit = raidxor_find_unit_bio(stripe, bio);
		});
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL error_no_unit
		CHECK_PLAIN(unit);

		md_error(conf->mddev, unit->rdev);
	error_no_unit: __attribute__((unused)) {}
	}

	WITHLOCKCONF(conf, {
	if ((--rxbio->remaining) == 0) {
		line->status = CACHE_LINE_UPTODATE;
		--cache->active_lines;
		/* TODO: wake up waiting threads */

		line->rxbio = NULL;
		kfree(rxbio);
	}
	});
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
 * Returns 0 if both bios have the same size and are layed out the same
 * way, else != 0.
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
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	/* since we have control over bioto and rxbio, every bio has size
	   M * CHUNK_SIZE with CHUNK_SIZE = N * PAGE_SIZE */
	unsigned long i;
	struct bio *biofrom;

	CHECK_ARGS3(bioto, rxbio, encoding);

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

/**
 * raidxor_error() - propagates a device error
 *
 */
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
		printk(KERN_ALERT "raidxor: disk failure on %s\n",
		       bdevname(rdev->bdev, buffer));
	}
	});
}

/**
 * raidxor_finish_lines() - tries to free some lines by writeback or dropping
 *
 */
static void raidxor_finish_lines(cache_t *cache)
{
	unsigned int i;
	cache_line_t *line;
	unsigned int freed = 0;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(cache->n_waiting > 0);
	CHECK_PLAIN_RET(cache->n_lines > 0);

	/* as long as there are more waiting slots than now free'd slots */
	for (i = 0; i < cache->n_lines && freed < cache->n_waiting; ++i) {
		line = &cache->lines[i];
		switch (line->status) {
		case CACHE_LINE_READY:
			if (line->waiting) break;
			/* shouldn't happen, nobody is waiting for this one, */
			++freed;
			break;
		case CACHE_LINE_CLEAN:
			/* shouldn't happen, so we just signal here anyway */
			printk(KERN_INFO "raidxor: got a clean line in"
			       " raidxor_finish_lines\n");
			++freed;
			break;
		case CACHE_LINE_UPTODATE:
			raidxor_cache_drop_line(cache, i);
			++freed;
			break;
		case CACHE_LINE_DIRTY:
			/* when the callback is invoked, the main thread is
			   woken up and eventually revisits this entry  */
			raidxor_cache_writeback_line(cache, i);
			break;
		case CACHE_LINE_LOAD_ME:
		case CACHE_LINE_LOADING:
		case CACHE_LINE_WRITEBACK:
		case CACHE_LINE_FAULTY:
		case CACHE_LINE_RECOVERY:
			/* can't do anything useful with these */
			break;
			/* there is no default */
		}
	}

	cache->n_waiting -= freed;
	if (freed > 0) {
		/* TODO: wakeup waiting processes */
#if 0
		wake_up(&cache->wait_for_line);
#endif
	}
}

/**
 * raidxor_handle_requests() - handles waiting requests for a cache line
 *
 *
 */
static void raidxor_handle_requests(cache_t *cache, unsigned int n_line)
{
	cache_line_t *line;
	struct bio *bio;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n_line < cache->n_lines);

	line = &cache->lines[n_line];
	CHECK_PLAIN_RET(line);
	CHECK_PLAIN_RET(line->status == CACHE_LINE_UPTODATE ||
			line->status == CACHE_LINE_DIRTY);

	/* requests are added at back, so take from front and handle */
	while ((bio = raidxor_cache_remove_request(cache, n_line))) {
		if (bio_data_dir(bio) == WRITE)
			raidxor_copy_bio_to_cache(cache, n_line, bio);
		else raidxor_copy_bio_from_cache(cache, n_line, bio);

		bio_endio(bio, 0);

		/* mark dirty */
		if (bio_data_dir(bio) == WRITE &&
		    line->status == CACHE_LINE_UPTODATE)
		{
			line->status = CACHE_LINE_DIRTY;
		}
	}
}

/**
 * raidxor_handle_line() - tries to do something with a cache line
 *
 * Returns 1 when we've done something, else 0.  Errors count as
 * 'done nothing' to prevent endless looping in those cases.
 */
static int raidxor_handle_line(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	cache_line_t *line;

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = &cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	/* if nobody wants something from this line, do nothing */
	if (!line->waiting) return 0;

	switch (line->status) {
	case CACHE_LINE_LOAD_ME:
		raidxor_cache_load_line(cache, n_line);
		goto out_done_something;
	case CACHE_LINE_FAULTY:
		raidxor_cache_recover(cache, n_line);
		goto out_done_something;
	case CACHE_LINE_UPTODATE:
	case CACHE_LINE_DIRTY:
		raidxor_handle_requests(cache, n_line);
		goto out_done_something;
	case CACHE_LINE_READY:
	case CACHE_LINE_RECOVERY:
	case CACHE_LINE_LOADING:
	case CACHE_LINE_WRITEBACK:
	case CACHE_LINE_CLEAN:
		/* no bugs, just can't do anything */
		break;
		/* no default */
	}

	return 0;
out_done_something:
	return 1;
}

/**
 * raidxord() - daemon thread
 *
 * Is started by the md level.  Takes requests from the queue and handles them.
 */
static void raidxord(mddev_t *mddev)
{
	unsigned int i;
	raidxor_conf_t *conf;
	cache_t *cache;
	unsigned int handled = 0;
	unsigned int done = 0;

	CHECK_ARG_RET(mddev);

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN_RET(conf);

	cache = conf->cache;
	CHECK_PLAIN_RET(cache);

	/* someone poked us.  see what we can do */
	printk(KERN_INFO "raidxor: raidxord active\n");

	WITHLOCKCONF(conf, {
	for (; !test_bit(CONF_STOPPING, &conf->flags) && !done;) {
		/* go through all cache lines, see if any waiting requests
		   can be handled */
		for (i = 0, done = 1; i < cache->n_lines; ++i) {
			/* only break if we have handled at least one line */
			if (raidxor_handle_line(cache, i))
				done = 0;
		}

		/* also, if somebody is waiting for a free line, try to make
		   one (or more) available.  freeing some lines doesn't count
		   for done above, so if we're done working on those lines
		   and we free two lines afterwards, the waiting processes
		   are notified and signal us back later on */
		if (cache->n_waiting > 0)
			raidxor_finish_lines(cache);

		/* give others the chance to do something */
		UNLOCKCONF(conf);
		LOCKCONF(conf);
	}
	});

	printk(KERN_INFO "raidxor: thread inactive, %u requests\n", handled);
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

	return 0;
}

/**
 * raidxor_wakeup_thread() - wakes the associated kernel thread
 *
 * Whenever we need something done, this will (re-)start the kernel thread
 * (see raidxord()).
 */
static void raidxor_wakeup_thread(raidxor_conf_t *conf)
{
	CHECK_ARG_RET(conf);
	md_wakeup_thread(conf->mddev->thread);
}

static void raidxor_align_sector_to_strip(raidxor_conf_t *conf,
					  stripe_t *stripe,
					  sector_t *sector)
{
	sector_t strip_sectors;
	sector_t mod;

	CHECK_ARG_RET(conf);
	CHECK_ARG_RET(stripe);
	CHECK_ARG_RET(sector);

	strip_sectors = (conf->chunk_size >> 9) * stripe->n_data_units;

	mod = *sector % strip_sectors;

	if (mod != 0)
		*sector -= mod;
}

/**
 * raidxor_check_bio_size_and_layout() - checks a bio for compatibility
 *
 * Checks whether the size is a multiple of PAGE_SIZE and each bio_vec
 * is exactly one page long and has an offset of 0.
 */
static int raidxor_check_bio_size_and_layout(raidxor_conf_t *conf,
					     struct bio *bio)
{
	unsigned int i;
	struct bio_vec *bvl;

	if ((bio->bi_size % PAGE_SIZE) != 0)
		return 1;

	bio_for_each_segment(bvl, bio, i) {
		if (bvl->bv_len != PAGE_SIZE)
			return 2;

		if (bvl->bv_offset != 0)
			return 3;
	}			

	return 0;
}

static int raidxor_make_request(struct request_queue *q, struct bio *bio)
{
	mddev_t *mddev;
	raidxor_conf_t *conf;
	cache_t *cache;
	stripe_t *stripe;
	struct bio_pair *split;
	unsigned int line;
	sector_t aligned_sector, strip_sectors;

#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(q);
	CHECK_ARG(bio);

	CHECK_PLAIN(mddev);
	mddev = q->queuedata;

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN(conf);

	cache = conf->cache;
	CHECK_PLAIN(cache);

	printk(KERN_EMERG "raidxor: got request\n");

	WITHLOCKCONF(conf, {
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_unlock

	CHECK_PLAIN(!raidxor_check_bio_size_and_layout(conf, bio));

	stripe = raidxor_sector_to_stripe(conf, bio->bi_sector, NULL);
	CHECK_PLAIN(stripe);
	strip_sectors = (conf->chunk_size >> 9) * stripe->n_data_units;


	aligned_sector = bio->bi_sector;

	/* round sector down to current or previous strip */
	raidxor_align_sector_to_strip(conf, stripe, &aligned_sector);

	/* set as offset to new base */
	bio->bi_sector = bio->bi_sector - aligned_sector;


	/* checked assumption is: aligned_sector is aligned to
	   strip/cache line, bio->bi_sector is the offset inside this strip
	   (and aligned to PAGE_SIZE) */

	CHECK_PLAIN(aligned_sector % (PAGE_SIZE >> 9) == 0);
	CHECK_PLAIN(aligned_sector % strip_sectors == 0);
	CHECK_PLAIN(bio->bi_sector % (PAGE_SIZE >> 9) == 0);

	if (bio->bi_sector + (bio->bi_size >> 9) > strip_sectors) {
		/* TODO: split bio */
		printk(KERN_EMERG "need to split request because "
		       "%llu > %llu\n",
		       (unsigned long long) (bio->bi_sector +
					     (bio->bi_size >> 9)),
		       (unsigned long long) strip_sectors);
		goto out_unlock;
	}

	/* look for matching line or otherwise available */
	if (!raidxor_cache_find_line(cache, aligned_sector, &line)) {
		/* if it's not in the cache and the stripe is flagged as error,
		   abort early, as we can do nothing */
		if (test_bit(STRIPE_ERROR, &stripe->flags))
			goto out_unlock;

		/* TODO: else if no line is available, wait for it ... */
		++cache->n_waiting;
		/* cache->wait_for_line; */
		/* TODO: detect CONF_STOPPING */
	}

	if (cache->lines[line].status == CACHE_LINE_CLEAN ||
	    cache->lines[line].status == CACHE_LINE_READY)
	{
		raidxor_cache_make_ready(cache, line);
		raidxor_cache_make_load_me(cache, line, aligned_sector);
	}
	/* TODO: which states are unacceptable? */
	/* pack the request somewhere in the cache */
	raidxor_cache_add_request(cache, line, bio);
	});

	raidxor_wakeup_thread(conf);

	return 0;
out_unlock: __attribute__((unused))
	UNLOCKCONF(conf);
out: __attribute__((unused))
	bio_io_error(bio);
	return 0;
}

#include "init.c"

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
