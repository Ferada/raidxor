/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

/* for do_div on 64bit machines */
#include <asm/div64.h>

/* for xor_blocks */
#include <linux/raid/xor.h>

#include "raidxor.h"

#include "params.c"
#include "utils.c"
#include "conf.c"

static int raidxor_cache_make_clean(cache_t *cache, unsigned int line)
{
 	CHECK_FUN(raidxor_cache_make_clean);

#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	if (cache->lines[line]->status == CACHE_LINE_CLEAN) return 0;
	CHECK_PLAIN_RET_VAL(cache->lines[line]->status == CACHE_LINE_READY);

	cache->lines[line]->status = CACHE_LINE_CLEAN;
	raidxor_cache_drop_line(cache, line);

	return 0;
}

static int raidxor_cache_make_ready(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	unsigned int i;
	cache_line_t *line;

 	CHECK_FUN(raidxor_cache_make_ready);

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	/* printk(KERN_EMERG "line status was %s\n", raidxor_cache_line_status(line)); */
	/* printk(KERN_EMERG "line at %p\n", line); */

	if (line->status == CACHE_LINE_READY) return 0;
	CHECK_PLAIN_RET_VAL(line->status == CACHE_LINE_CLEAN || 
			    line->status == CACHE_LINE_UPTODATE);

	/* printk(KERN_EMERG "cache->n_buffers == %u, cache->n_red_buffers == %u\n", */
	/*       cache->n_buffers, cache->n_red_buffers); */

	if (line->status == CACHE_LINE_CLEAN)
		for (i = 0; i < cache->n_buffers + cache->n_red_buffers; ++i) {
			/* printk(KERN_EMERG "line->buffers[%u] at %p, before %p\n", i, &line->buffers[i], line->buffers[i]); */
			if (!(line->buffers[i] = alloc_page(GFP_NOIO))) {
				printk(KERN_EMERG "page allocation failed for line %u\n", n_line);
				goto out_free_pages;
			}
			/* printk(KERN_EMERG "line->buffers[%u] is now %p\n", i, line->buffers[i]); */
		}
	line->status = CACHE_LINE_READY;


	return 0;
out_free_pages:
	line->status = CACHE_LINE_READY;
	raidxor_cache_make_clean(cache, n_line);
	return 1;
}

static int raidxor_cache_make_load_me(cache_t *cache, unsigned int line,
				      sector_t sector)
{
	CHECK_FUN(raidxor_cache_make_load_me);

#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(line < cache->n_lines);

	if (cache->lines[line]->status == CACHE_LINE_LOAD_ME) return 0;
	CHECK_PLAIN_RET_VAL(cache->lines[line]->status == CACHE_LINE_READY);

	cache->lines[line]->status = CACHE_LINE_LOAD_ME;
	cache->lines[line]->sector = sector;


	return 0;
}

/**
 * raidxor_cache_abort_line() - aborts a line
 *
 * Aborts all waiting requests, drops sector information and marks READY.
 */
static void raidxor_cache_abort_line(cache_t *cache, unsigned int line)
{
	raidxor_cache_abort_requests(cache, line);
	cache->lines[line]->status = CACHE_LINE_UPTODATE;
	raidxor_cache_make_ready(cache, line);
}

/**
 * raidxor_valid_decoding() - checks for necessary equation(s)
 *
 * Returns 1 if everything is okay, else 0.
 */
static unsigned int raidxor_valid_decoding(cache_t *cache, unsigned int n_line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	cache_line_t *line;
	unsigned int i;
	stripe_t *stripe;
	raidxor_bio_t *rxbio;

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	rxbio = line->rxbio;
	CHECK_PLAIN_RET_VAL(rxbio);

	stripe = rxbio->stripe;
	CHECK_PLAIN_RET_VAL(stripe);

	for (i = 0; i < stripe->n_units; ++i)
		if (test_bit(Faulty, &stripe->units[i]->rdev->flags) &&
		    !stripe->units[i]->decoding)
			return 0;

	return 1;
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

static void raidxor_cache_commit_bio(cache_t *cache, unsigned int n)
{
	unsigned int i;
	raidxor_bio_t *rxbio;
	stripe_t *stripe;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n < cache->n_lines);
	CHECK_PLAIN_RET(cache->lines[n]);

	rxbio = cache->lines[n]->rxbio;
	CHECK_PLAIN_RET(rxbio);

	stripe = rxbio->stripe;
	CHECK_PLAIN_RET(stripe);

	for (i = 0; i < rxbio->n_bios; ++i)
		if (!test_bit(Faulty, &stripe->units[i]->rdev->flags))
			generic_make_request(rxbio->bios[i]);
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
	unsigned int i, j, k, l, n_chunk_mult;

 	CHECK_FUN(raidxor_cache_load_line);

	CHECK_ARG(cache);
	CHECK_PLAIN(n < cache->n_lines);

	conf = cache->conf;
	CHECK_PLAIN(conf);

	line = cache->lines[n];
	CHECK_PLAIN(line->status == CACHE_LINE_LOAD_ME);

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK_PLAIN(stripe);
	/* unrecoverable error, abort */
	if (test_bit(STRIPE_ERROR, &stripe->flags)) {
		CHECK_BUG("stripe with error code in load_line");
		goto out;
	}

	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	rxbio->cache = cache;
	rxbio->stripe = stripe;
	rxbio->line = n;
	rxbio->remaining = rxbio->n_bios;

	line->rxbio = rxbio;

	CHECK_LINE;

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0, l = 0; i < rxbio->n_bios; ++i) {
		/* printk(KERN_EMERG "i = %u, l = %u, rxbio->n_bios = %u, stripe->n_units = %u\n", i, l, rxbio->n_bios, stripe->n_units); */
		/* we also load the redundant pages */

		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		bio->bi_rw = READ;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_load_line;

		/* bio->bi_sector = actual_sector / stripe->n_data_units + */
		/* 	stripe->units[i]->rdev->data_offset; */
		bio->bi_sector = actual_sector;
		do_div(bio->bi_sector, stripe->n_data_units);
		bio->bi_sector += stripe->units[i]->rdev->data_offset;

		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		/* printk(KERN_EMERG "bio->bi_size = %u, bio = %p\n", bio->bi_size, bio); */

		/* printk(KERN_EMERG "cache->n_buffers = %d, n_red_buffers = %d\n",
		       cache->n_buffers, cache->n_red_buffers); */
		/* assign pages */
		bio->bi_vcnt = n_chunk_mult;
		for (j = 0; j < n_chunk_mult; ++j) {
			if (stripe->units[i]->redundant) {
				k = cache->n_buffers + l * n_chunk_mult + j;
				/* printk(KERN_EMERG "[%d], red k = %d\n", j, k); */
			}
			else {
				k = i * n_chunk_mult + j;
				/* printk(KERN_EMERG "[%d], nonred k = %d\n", j, k); */
			}

			CHECK_PLAIN(line->buffers[k]);
			bio->bi_io_vec[j].bv_page = line->buffers[k];

			bio->bi_io_vec[j].bv_len = PAGE_SIZE;
			bio->bi_io_vec[j].bv_offset = 0;
		}

		if (stripe->units[i]->redundant)
			++l;

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags)) {
			/* printk(KERN_EMERG "got faulty drive during load\n"); */
			--rxbio->remaining;
			if (!stripe->units[i]->redundant)
				rxbio->faulty = 1;
		}
	}

	++cache->active_lines;

	/* printk(KERN_EMERG "with %d bios\n", rxbio->n_bios); */

	line->status = CACHE_LINE_LOADING;

	return 0;
out_free_bio: __attribute__((unused))
	raidxor_free_bio(rxbio);
out: __attribute__((unused))
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
	unsigned int i, j, k, l, n_chunk_mult;
	struct bio *bio;
	raidxor_conf_t *conf = cache->conf;

 	CHECK_FUN(raidxor_cache_writeback_line);

	CHECK_ARG(cache);
	CHECK_PLAIN(n < cache->n_lines);

	line = cache->lines[n];
	CHECK_PLAIN(line->status == CACHE_LINE_DIRTY);

	stripe = raidxor_sector_to_stripe(conf, line->sector,
					  &actual_sector);
	CHECK_PLAIN(stripe);

	rxbio = raidxor_alloc_bio(stripe->n_units);
	CHECK_PLAIN(rxbio);
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_free_bio

	rxbio->cache = cache;
	rxbio->stripe = stripe;
	rxbio->line = n;
	rxbio->remaining = rxbio->n_bios;

	line->rxbio = rxbio;

	n_chunk_mult = cache->n_chunk_mult;

	for (i = 0, l = 0; i < rxbio->n_bios; ++i) {
		/* only one chunk */
		rxbio->bios[i] = bio = bio_alloc(GFP_NOIO, cache->n_chunk_mult);
		CHECK_ALLOC(rxbio->bios[i]);

		bio->bi_rw = WRITE;
		bio->bi_private = rxbio;
		bio->bi_bdev = stripe->units[i]->rdev->bdev;
		bio->bi_end_io = raidxor_end_writeback_line;

		/* bio->bi_sector = actual_sector / stripe->n_data_units + */
		/* 	stripe->units[i]->rdev->data_offset; */
		bio->bi_sector = actual_sector;
		do_div(bio->bi_sector, stripe->n_data_units);
		bio->bi_sector += stripe->units[i]->rdev->data_offset;

		bio->bi_size = n_chunk_mult * PAGE_SIZE;

		bio->bi_vcnt = n_chunk_mult;
		/* assign pages */
		for (j = 0; j < n_chunk_mult; ++j) {
			if (stripe->units[i]->redundant) {
				k = cache->n_buffers + l * n_chunk_mult + j;
				/* printk(KERN_EMERG "[%d], red k = %d\n", j, k); */
			}
			else {
				k = i * n_chunk_mult + j;
				/* printk(KERN_EMERG "[%d], nonred k = %d\n", j, k); */
			}

			bio->bi_io_vec[j].bv_page = line->buffers[k];
			bio->bi_io_vec[j].bv_len = PAGE_SIZE;
			bio->bi_io_vec[j].bv_offset = 0;
		}

		if (stripe->units[i]->redundant)
			++l;

		if (test_bit(Faulty, &stripe->units[i]->rdev->flags)) {
			/* printk(KERN_INFO "raidxor: got a faulty drive during writeback\n"); */
			--rxbio->remaining;
			if (!stripe->units[i]->redundant)
				rxbio->faulty = 1;
		}
	}
	
	for (i = 0; i < rxbio->n_bios; ++i) {
		if (!stripe->units[i]->redundant) continue;
		if (raidxor_xor_combine_encode(rxbio->bios[i], rxbio,
					       stripe->units[i]->encoding))
			goto out_free_bio;
	}

	++cache->active_lines;

	line->status = CACHE_LINE_WRITEBACK;

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
	unsigned int index, data_index, wake = 0;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_end_load_line);

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	stripe = rxbio->stripe;

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	index = raidxor_bio_index(rxbio, bio, &data_index);

	/* CHECK_LINE; */

	if (error) {
		WITHLOCKCONF(conf, flags, {
		if (!stripe->units[index]->redundant)
			rxbio->faulty = 1;
		});
		md_error(conf->mddev, stripe->units[index]->rdev);
	}

	CHECK_LINE;

	WITHLOCKCONF(conf, flags, {
	if ((--rxbio->remaining) == 0) {
		/* printk(KERN_EMERG "last callback for line %d, waking thread, %u faulty\n", */
		/*        rxbio->line, rxbio->faulty); */
		if (rxbio->faulty)
			line->status = CACHE_LINE_FAULTY;
		else  {
			line->status = CACHE_LINE_UPTODATE;
			line->rxbio = NULL;
			kfree(rxbio);
		}
		--cache->active_lines;
		wake = 1;
	}
	});

	if (wake) raidxor_wakeup_thread(conf);
}

static void raidxor_end_writeback_line(struct bio *bio, int error)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	raidxor_bio_t *rxbio;
	raidxor_conf_t *conf;
	cache_t *cache;
	cache_line_t *line;
	stripe_t *stripe;
	unsigned int index, data_index, wake = 0;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_end_writeback_line);

	CHECK_ARG_RET(bio);

	rxbio = (raidxor_bio_t *)(bio->bi_private);
	CHECK_PLAIN_RET(rxbio);

	cache = rxbio->cache;
	CHECK_PLAIN_RET(cache);

	CHECK_PLAIN_RET(rxbio->line < cache->n_lines);

	line = cache->lines[rxbio->line];
	CHECK_PLAIN_RET(line);

	conf = rxbio->cache->conf;
	CHECK_PLAIN_RET(conf);

	stripe = rxbio->stripe;
	CHECK_PLAIN_RET(stripe);

	index = raidxor_bio_index(rxbio, bio, &data_index);

	if (error)
		md_error(conf->mddev, stripe->units[index]->rdev);

	WITHLOCKCONF(conf, flags, {
	if ((--rxbio->remaining) == 0) {
		line->status = CACHE_LINE_UPTODATE;

		line->rxbio = NULL;
		kfree(rxbio);

		--cache->active_lines;
		wake = 1;
	}
	});

	if (wake) raidxor_wakeup_thread(conf);
}

/**
 * raidxor_xor_single() - xors the buffers of two bios together
 *
 * Both bios have to be of the same size and layout.
 */
static void raidxor_xor_single(struct bio *bioto, struct bio *biofrom)
{
	unsigned int i;
	struct bio_vec *bvto, *bvfrom;
	unsigned char *tomapped, *frommapped;
	unsigned char *toptr, *fromptr;
	void *srcs[1];

	for (i = 0; i < bioto->bi_vcnt; ++i) {
		bvto = bio_iovec_idx(bioto, i);
		bvfrom = bio_iovec_idx(biofrom, i);

		if (bvto->bv_len != PAGE_SIZE)
			CHECK_BUG("buffer has not length PAGE_SIZE");

		tomapped = (unsigned char *) kmap(bvto->bv_page);
		frommapped = (unsigned char *) kmap(bvfrom->bv_page);

		toptr = tomapped + bvto->bv_offset;
		fromptr = frommapped + bvfrom->bv_offset;

		/* printk(KERN_EMERG "combining buffer %p to buffer %p\n", fromptr, toptr); */

#if 0
		for (j = 0; j < bvto->bv_len; ++j, ++toptr, ++fromptr) {
			*toptr ^= *fromptr;
		}
#else
		srcs[0] = fromptr;
		xor_blocks(1, PAGE_SIZE, toptr, srcs);
#endif

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

	/* have the same number of bio_vecs, */
	if (x->bi_vcnt != y->bi_vcnt)
		return 1;

	/* and those are of the same length, pairwise */
	for (i = 0; i < x->bi_vcnt; ++i) {
		/* FIXME: if this not printd, the test fails */
		/* printk(KERN_INFO "comparing %d and %d\n",
		       x->bi_io_vec[i].bv_len, y->bi_io_vec[i].bv_len); */
		if (x->bi_io_vec[i].bv_len != y->bi_io_vec[i].bv_len)
			return 2;
	}

	return 0;
}

static int raidxor_xor_combine_decode(struct bio *bioto, raidxor_bio_t *rxbio,
				      decoding_t *decoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	/* since we have control over bioto and rxbio, every bio has size
	   M * CHUNK_SIZE with CHUNK_SIZE = N * PAGE_SIZE */
	unsigned int i, err;
	struct bio *biofrom;

	CHECK_FUN(raidxor_xor_combine_decode);

	CHECK_ARGS3(bioto, rxbio, decoding);

	/* copying first bio buffers */
	biofrom = raidxor_find_bio(rxbio, decoding->units[0]);
	raidxor_copy_bio(bioto, biofrom);

	/* then, xor the other buffers to the first one */
	for (i = 1; i < decoding->n_units; ++i) {
		/* printk(KERN_EMERG "decoding unit %u out of %d\n", i,
		       decoding->n_units); */
		/* search for the right bio */
		biofrom = raidxor_find_bio(rxbio, decoding->units[i]);

		if (!biofrom) {
			printk(KERN_EMERG "raidxor: didn't find bio in"
			       " raidxor_xor_combine_decode\n");
			goto out;
		}

#ifdef RAIDXOR_DEBUG
		if ((err = raidxor_check_same_size_and_layout(bioto, biofrom))) {
			printk(KERN_EMERG "raidxor: bioto and biofrom"
			       " differ in size and/or layout: %u\n", err);
			goto out;
		}
#endif

		/* combine the data */
		raidxor_xor_single(bioto, biofrom);
	}

	return 0;
out:
	return 1;
}

/**
 * raidxor_xor_combine_encode() - xors a number of resources together
 *
 * Takes a master request and combines the request inside the rxbio together
 * using the given encoding for the unit.
 *
 * Returns 1 on error (bioto still might be touched in this case).
 */
static int raidxor_xor_combine_encode(struct bio *bioto, raidxor_bio_t *rxbio,
				      encoding_t *encoding)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	/* since we have control over bioto and rxbio, every bio has size
	   M * CHUNK_SIZE with CHUNK_SIZE = N * PAGE_SIZE */
	unsigned int i, err;
	struct bio *biofrom;

	CHECK_FUN(raidxor_xor_combine_encode);

	CHECK_ARGS3(bioto, rxbio, encoding);

	/* copying first bio buffers */
	biofrom = raidxor_find_bio(rxbio, encoding->units[0]);
	raidxor_copy_bio(bioto, biofrom);

	/* then, xor the other buffers to the first one */
	for (i = 1; i < encoding->n_units; ++i) {
		/* printk(KERN_EMERG "encoding unit %u out of %d\n", i,
		       encoding->n_units); */
		/* search for the right bio */
		biofrom = raidxor_find_bio(rxbio, encoding->units[i]);

		if (!biofrom) {
			printk(KERN_EMERG "raidxor: didn't find bio in"
			       " raidxor_xor_combine_encode\n");
			goto out;
		}

#ifdef RAIDXOR_DEBUG
		if ((err = raidxor_check_same_size_and_layout(bioto, biofrom))) {
			printk(KERN_EMERG "raidxor: bioto and biofrom"
			       " differ in size and/or layout: %u\n", err);
			goto out;
		}
#endif

		/* combine the data */
		raidxor_xor_single(bioto, biofrom);
	}

	return 0;
out:
	return 1;
}

/**
 * raidxor_cache_recover() - tries to recover a cache line
 *
 * Since the read buffers are available, we can use them to calculate
 * the missing data.
 */
static void raidxor_cache_recover(cache_t *cache, unsigned int n_line)
{
	cache_line_t *line;
	raidxor_conf_t *conf;
	raidxor_bio_t *rxbio;
	unsigned int i;
	stripe_t *stripe;
	unsigned long flags = 0;

	CHECK_FUN(raidxor_cache_recover);

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET(line);

	rxbio = line->rxbio;
	CHECK_PLAIN_RET(rxbio);

	stripe = rxbio->stripe;
	CHECK_PLAIN_RET(stripe);

	conf = cache->conf;
	CHECK_PLAIN_RET(conf);

	WITHLOCKCONF(conf, flags, {
	if (!raidxor_valid_decoding(cache, n_line))
		goto out;

	line->status = CACHE_LINE_RECOVERY;

	/* decoding using direct style */
	for (i = 0; i < rxbio->n_bios; ++i) {
		if (test_bit(Faulty, &stripe->units[i]->rdev->flags) &&
		    raidxor_xor_combine_decode(rxbio->bios[i], rxbio,
					       stripe->units[i]->decoding))
			goto out;
	}
	/* printk(KERN_EMERG "decoded\n"); */

	line->status = CACHE_LINE_UPTODATE;
	});
	return;
out:
	/* drop this line if an error occurs or we can't recover */
	raidxor_cache_abort_line(cache, n_line);
	UNLOCKCONF(conf, flags);
}

static void raidxor_invalidate_decoding(raidxor_conf_t *conf,
					disk_info_t *unit)
{
	stripe_t *stripe;
	unsigned int i;

	CHECK_ARG_RET(conf);
	CHECK_ARG_RET(unit);

	stripe = unit->stripe;
	CHECK_PLAIN_RET(stripe);

	if (unit->decoding) {
		CHECK_BUG("unit has decoding, although it shouldn't have one");
		raidxor_safe_free_decoding(unit);
	}

	for (i = 0; i < stripe->n_units; ++i)
		if (stripe->units[i]->decoding &&
		    raidxor_find_unit_decoding(stripe->units[i]->decoding,
					       unit)) {
			raidxor_safe_free_decoding(stripe->units[i]);
			/* printk(KERN_EMERG "raidxor: unit %u requests decoding"
			       " information\n",
			       raidxor_unit_to_index(conf, stripe->units[i])); */
		}

	printk(KERN_CRIT "raidxor: raid %s needs new decoding information\n",
	       mdname(conf->mddev));
}

/**
 * raidxor_error() - propagates a device error
 *
 */
static void raidxor_error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	unsigned long flags = 0;
	char buffer[BDEVNAME_SIZE];
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	disk_info_t *unit = raidxor_find_unit_conf_rdev(conf, rdev);

	WITHLOCKCONF(conf, flags, {
	if (!test_bit(Faulty, &rdev->flags)) {
		/* escalate error */
		set_bit(Faulty, &rdev->flags);
		set_bit(STRIPE_FAULTY, &unit->stripe->flags);
		set_bit(CONF_FAULTY, &conf->flags);
		raidxor_invalidate_decoding(conf, unit);
		printk(KERN_CRIT "raidxor: disk failure on %s\n",
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
	unsigned long flags = 0;

	CHECK_FUN(raidxor_finish_lines);

	CHECK_ARG_RET(cache);

	WITHLOCKCONF(cache->conf, flags, {
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_PLAIN(cache->n_waiting > 0);
	CHECK_PLAIN(cache->n_lines > 0);

	/* as long as there are more waiting slots than now free'd slots */
	for (i = 0; i < cache->n_lines && freed < cache->n_waiting; ++i) {
		line = cache->lines[i];
		switch (line->status) {
		case CACHE_LINE_READY:
#ifdef RAIDXOR_DEBUG
			if (line->waiting)
				printk(KERN_EMERG "line %u with state READY has waiting in finish_lines\n", i);
			else
				printk(KERN_EMERG "line %u with state READY has no waiting in finish_lines\n", i);
#endif
			/* can only happen if we stop the raid */
			break;
		case CACHE_LINE_CLEAN:
			if (line->waiting) {
#ifdef RAIDXOR_DEBUG
				printk(KERN_EMERG "line %u with STATE CLEAN has waiting in finish_lines\n", i);
#endif
				break;
			}
			++freed;
			break;
		case CACHE_LINE_UPTODATE:
			if (line->waiting) {
#ifdef RAIDXOR_DEBUG
				printk(KERN_EMERG "line %u with STATE UPTODATE has waiting in finish_lines\n", i);
#endif
				break;
			}
			raidxor_cache_make_ready(cache, i);
			++freed;
			break;
		case CACHE_LINE_DIRTY:
			if (line->waiting) {
#ifdef RAIDXOR_DEBUG
				printk(KERN_EMERG "line %u with STATE DIRTY has waiting in finish_lines\n", i);
#endif
				break;
			}
			/* when the callback is invoked, the main thread is
			   woken up and eventually revisits this entry  */
			if (!raidxor_cache_writeback_line(cache, i)) {
				UNLOCKCONF(cache->conf, flags);
				raidxor_cache_commit_bio(cache, i);
				LOCKCONF(cache->conf, flags);
			}
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

	/* printk(KERN_EMERG "freed %u lines\n", freed); */

out: __attribute__((unused))
	do {} while (0);

	});

	for (i = 0; i < freed; ++i)
		raidxor_signal_empty_line(cache->conf);
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
	unsigned long flags;

	CHECK_FUN(raidxor_handle_requests);

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET(line);

	WITHLOCKCONF(cache->conf, flags, {
	CHECK_PLAIN_RET(line->status == CACHE_LINE_UPTODATE ||
			line->status == CACHE_LINE_DIRTY);

	/* requests are added at back, so take from front and handle */
	while ((bio = raidxor_cache_remove_request(cache, n_line))) {
		if (bio_data_dir(bio) == WRITE)
			raidxor_copy_bio_to_cache(cache, n_line, bio);
		else raidxor_copy_bio_from_cache(cache, n_line, bio);

		/* mark dirty */
		if (bio_data_dir(bio) == WRITE &&
		    line->status == CACHE_LINE_UPTODATE)
		{
			line->status = CACHE_LINE_DIRTY;
		}

		UNLOCKCONF(cache->conf, flags);
		bio_endio(bio, 0);
		LOCKCONF(cache->conf, flags);
	}
	});
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
	unsigned long flags;
	unsigned int commit = 0, done = 0;

	/* CHECK_FUN(raidxor_handle_line); */

	CHECK_ARG_RET_VAL(cache);
	CHECK_PLAIN_RET_VAL(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_VAL(line);

	WITHLOCKCONF(cache->conf, flags, {

	/* if nobody wants something from this line, do nothing */
	if (!line->waiting) goto out_unlock;

	switch (line->status) {
	case CACHE_LINE_LOAD_ME:
		commit = !raidxor_cache_load_line(cache, n_line);
		done = 1;
		break;
	case CACHE_LINE_FAULTY:
		UNLOCKCONF(cache->conf, flags);
		raidxor_cache_recover(cache, n_line);
		done = 1;
		goto break_unlocked;
	case CACHE_LINE_UPTODATE:
	case CACHE_LINE_DIRTY:
		/* printk(KERN_EMERG "line %d still had %d requests\n",
		       n_line,
		       raidxor_cache_line_length_requests(cache, n_line)); */
		UNLOCKCONF(cache->conf, flags);
		raidxor_handle_requests(cache, n_line);
		done = 1;
		goto break_unlocked;
	case CACHE_LINE_READY:
	case CACHE_LINE_RECOVERY:
	case CACHE_LINE_LOADING:
	case CACHE_LINE_WRITEBACK:
	case CACHE_LINE_CLEAN:
		/* no bugs, just can't do anything */
		break;
		/* no default */
	}

	});
break_unlocked:

	if (commit) raidxor_cache_commit_bio(cache, n_line);

	return done;
out_unlock:
	UNLOCKCONF(cache->conf, flags);
	return 0;
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
#ifdef RAIDXOR_DEBUG
	unsigned long flags = 0;
#endif

	CHECK_ARG_RET(mddev);

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN_RET(conf);

	cache = conf->cache;
	CHECK_PLAIN_RET(cache);

	/* someone poked us.  see what we can do */
	pr_debug("raidxor: raidxord active\n");

	for (; !done;) {
		/* go through all cache lines, see if any waiting requests
		   can be handled */
		for (i = 0, done = 1; i < cache->n_lines; ++i) {
			/* only break if we have handled at least one line */
			if (raidxor_handle_line(cache, i)) {
				++handled;
				done = 0;
			}
		}

		/* also, if somebody is waiting for a free line, try to make
		   one (or more) available.  freeing some lines doesn't count
		   for done above, so if we're done working on those lines
		   and we free two lines afterwards, the waiting processes
		   are notified and signal us back later on */

		if (cache->n_waiting > 0) {
			raidxor_finish_lines(cache);
		}
	}

#ifdef RAIDXOR_DEBUG
	WITHLOCKCONF(conf, flags, {
	raidxor_cache_print_status(cache);
	});
#endif
	pr_debug("raidxor: thread inactive, %u lines handled\n", handled);
}

static void raidxor_unplug(struct request_queue *q)
{
	mddev_t *mddev = q->queuedata;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned int i;
	struct request_queue *r_queue;

	for (i = 0; i < conf->n_units; i++) {
		r_queue = bdev_get_queue(conf->units[i].rdev->bdev);

		blk_unplug(r_queue);
	}
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
		printk(KERN_EMERG "raidxor: %s: raid level not set to xor (%d)\n",
		       mdname(mddev), mddev->level);
		goto out_inval;
	}

	if (mddev->chunk_size < PAGE_SIZE) {
		printk(KERN_EMERG "raidxor: chunk_size must be at least "
		       "PAGE_SIZE but %d < %ld\n",
		       mddev->chunk_size, PAGE_SIZE);
		goto out_inval;
	}

	printk(KERN_EMERG "raidxor: raid set %s active with %d disks\n",
	       mdname(mddev), mddev->raid_disks);

	if (mddev->raid_disks < 1)
		goto out_inval;

	conf = kzalloc(sizeof(raidxor_conf_t) +
		       sizeof(struct disk_info) * mddev->raid_disks, GFP_KERNEL);
	mddev->private = conf;
	if (!conf) {
		printk(KERN_EMERG "raidxor: couldn't allocate memory for %s\n",
		       mdname(mddev));
		goto out;
	}

	conf->configured = 0;
	conf->mddev = mddev;
	conf->chunk_size = mddev->chunk_size;
	conf->units_per_resource = 0;
	conf->resources_per_stripe = 0;
	conf->n_resources = 0;
	conf->resources = NULL;
	conf->n_stripes = 0;
	conf->stripes = NULL;
	conf->n_units = mddev->raid_disks;

	printk(KERN_EMERG "whoo, setting hardsect size to %d\n", 4096);
	blk_queue_hardsect_size(mddev->queue, 4096);

	spin_lock_init(&conf->device_lock);
	mddev->queue->queue_lock = &conf->device_lock;
	mddev->queue->unplug_fn = raidxor_unplug;

	size = -1; /* rdev->size is in sectors, that is 1024 byte */

	i = conf->n_units - 1;
	rdev_for_each(rdev, tmp, mddev) {
		size = min(size, rdev->size);

		printk(KERN_EMERG "raidxor: device %lu rdev %s, %llu blocks\n",
		       i, bdevname(rdev->bdev, buffer),
		       (unsigned long long) rdev->size * 2);
		conf->units[i].rdev = rdev;
		conf->units[i].redundant = -1;

		--i;
	}
	if (size == -1)
		goto out_free_conf;

	printk(KERN_EMERG "raidxor: used component size: %llu sectors\n",
	       (unsigned long long) size & ~(conf->chunk_size / 1024 - 1));

	/* used component size in sectors, multiple of chunk_size ... */
	mddev->size = size & ~(conf->chunk_size / 1024 - 1);
	/* exported size in blocks, will be initialised later */
	mddev->array_sectors = 0;

	/* Ok, everything is just fine now */
	if (sysfs_create_group(&mddev->kobj, &raidxor_attrs_group)) {
		printk(KERN_EMERG
		       "raidxor: failed to create sysfs attributes for %s\n",
		       mdname(mddev));
		goto out_free_conf;
	}

	mddev->thread = md_register_thread(raidxord, mddev, "%s_raidxor");
	if (!mddev->thread) {
		printk(KERN_EMERG
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
	unsigned long flags = 0;

	WITHLOCKCONF(conf, flags, {
	set_bit(CONF_STOPPING, &conf->flags);
	raidxor_wait_for_no_active_lines(conf);
	raidxor_wait_for_writeback(conf);
	});

	md_unregister_thread(mddev->thread);
	mddev->thread = NULL;

	sysfs_remove_group(&mddev->kobj, &raidxor_attrs_group);
	blk_sync_queue(mddev->queue);

	mddev_to_conf(mddev) = NULL;
	raidxor_safe_free_conf(conf);
	kfree(conf);

	return 0;
}

static void raidxor_align_sector_to_strip(raidxor_conf_t *conf,
					  stripe_t *stripe,
					  sector_t *sector)
{
	sector_t strip_sectors;
	sector_t div;
	sector_t mod;

	CHECK_ARG_RET(conf);
	CHECK_ARG_RET(stripe);
	CHECK_ARG_RET(sector);

	strip_sectors = (conf->chunk_size >> 9) * stripe->n_data_units;

	/* mod = *sector % strip_sectors; */
	div = *sector;
	mod = do_div(div, strip_sectors);

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
	sector_t div, mod;

	div = bio->bi_size;
	mod = do_div(div, PAGE_SIZE);

	if (mod != 0)
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
	unsigned int line;
	sector_t aligned_sector, strip_sectors, mod, div;
	unsigned long flags = 0;

#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	CHECK_ARG(q);
	CHECK_ARG(bio);

	mddev = q->queuedata;
	CHECK_PLAIN(mddev);

	conf = mddev_to_conf(mddev);
	CHECK_PLAIN(conf);

	cache = conf->cache;
	CHECK_PLAIN(cache);

	/* printk(KERN_EMERG "raidxor: got request\n"); */

	WITHLOCKCONF(conf, flags, {
/*	spin_lock_irq(&conf->device_lock); */
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out_unlock

	/* printk(KERN_EMERG "virtual sector %llu, length %u\n",
	       (unsigned long long) bio->bi_sector, bio->bi_size); */

	if (test_bit(CONF_STOPPING, &conf->flags))
		goto out_unlock;

	CHECK_PLAIN(!raidxor_check_bio_size_and_layout(conf, bio));

	stripe = raidxor_sector_to_stripe(conf, bio->bi_sector, NULL);
	CHECK_PLAIN(stripe);
	strip_sectors = (conf->chunk_size >> 9) * stripe->n_data_units;

	aligned_sector = bio->bi_sector;

	/* round sector down to current or previous strip */
	raidxor_align_sector_to_strip(conf, stripe, &aligned_sector);

	/* set as offset to new base */
	bio->bi_sector = bio->bi_sector - aligned_sector;

	/* printk(KERN_EMERG "aligned_sector %llu, bio->bi_sector %llu\n",
	       aligned_sector, bio->bi_sector); */

	/* checked assumption is: aligned_sector is aligned to
	   strip/cache line, bio->bi_sector is the offset inside this strip
	   (and aligned to PAGE_SIZE) */

	div = aligned_sector;
	mod = do_div(div, PAGE_SIZE >> 9);
	CHECK_PLAIN(mod == 0);

	div = aligned_sector;
	mod = do_div(div, strip_sectors);
	CHECK_PLAIN(mod == 0);

	div = bio->bi_sector;
	mod = do_div(div, PAGE_SIZE >> 9);
	CHECK_PLAIN(mod == 0);

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

		/* printk(KERN_EMERG "waiting for empty line\n"); */

		raidxor_wait_for_empty_line(conf);

		if (test_bit(CONF_STOPPING, &conf->flags) ||
		    test_bit(STRIPE_ERROR, &stripe->flags))
			goto out_unlock;
	}

	if (!raidxor_cache_find_line(cache, aligned_sector, &line))
		goto out_unlock;

	/* printk(KERN_EMERG "found available line %u\n", line); */

	if (cache->lines[line]->status == CACHE_LINE_CLEAN ||
	    cache->lines[line]->status == CACHE_LINE_READY)
	{
		if (raidxor_cache_make_ready(cache, line)) {
			CHECK_BUG("raidxor_cache_make_ready failed mysteriously");
			goto out_unlock;
		}
		if (raidxor_cache_make_load_me(cache, line, aligned_sector)) {
			CHECK_BUG("raidxor_cache_make_load_me failed mysteriously");
			goto out_unlock;
		}
	}

	/* TODO: which states are unacceptable? */
	/* pack the request somewhere in the cache */
	raidxor_cache_add_request(cache, line, bio);
	});
/*	spin_unlock_irq(&conf->device_lock); */

	raidxor_wakeup_thread(conf);

	return 0;
out_unlock: __attribute__((unused))
	UNLOCKCONF(conf, flags);
/*	spin_unlock_irq(&conf->device_lock); */
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
