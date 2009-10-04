/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

/**
 * raidxor_fill_page() - fills page with a value
 *
 * Copies value length times into the page buffer.
 */
static void raidxor_fill_page(struct page *page, unsigned char value,
			      unsigned long length)
{
	unsigned char *data = kmap(page);
	memset(data, value, length);
	kunmap(page);
}

static const char * raidxor_cache_line_status(cache_line_t *line)
{
	CHECK_ARG_RET_NULL(line);

	switch (line->status) {
	case CACHE_LINE_CLEAN:
		return "CACHE_LINE_CLEAN";
	case CACHE_LINE_READYING:
		return "CACHE_LINE_READYING";
	case CACHE_LINE_READY:
		return "CACHE_LINE_READY";
	case CACHE_LINE_LOAD_ME:
		return "CACHE_LINE_LOAD_ME";
	case CACHE_LINE_LOADING:
		return "CACHE_LINE_LOADING";
	case CACHE_LINE_UPTODATE:
		return "CACHE_LINE_UPTODATE";
	case CACHE_LINE_DIRTY:
		return "CACHE_LINE_DIRTY";
	case CACHE_LINE_WRITEBACK:
		return "CACHE_LINE_WRITEBACK";
	case CACHE_LINE_FAULTY:
		return "CACHE_LINE_FAULTY";
	case CACHE_LINE_RECOVERY:
		return "CACHE_LINE_RECOVERY";
	}

	return "UNKNOWN!";
}

static void raidxor_cache_print_status(cache_t *cache)
{
	unsigned int i;

	printk(CHECK_LEVEL "cache with %u waiting, %u active lines\n",
	       cache->n_waiting, cache->active_lines);

	for (i = 0; i < cache->n_lines; ++i) {
		printk(CHECK_LEVEL "line %u: %s at sector %llu, has %s request\n", i,
		       raidxor_cache_line_status(cache->lines[i]),
		       (unsigned long long) cache->lines[i]->sector,
		       cache->lines[i]->waiting ? "has at least one" : "has no");
	}
}

/**
 * raidxor_cache_add_request() - adds request at back
 */
static void raidxor_cache_add_request(cache_t *cache, unsigned int n_line,
				      struct bio *bio)
{
	cache_line_t *line;
	struct bio *previous;
	CHECK_ARG_RET(cache);
	CHECK_ARG_RET(bio);
	CHECK_PLAIN_RET(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET(line);

	previous = line->waiting;
	if (!previous) line->waiting = bio;
	else {
		while (previous->bi_next) {
			previous = previous->bi_next;
		}

		previous->bi_next = bio;
	}
}

/**
 * raidxor_cache_remove_request() - pops request from front
 */
static struct bio * raidxor_cache_remove_request(cache_t *cache,
						 unsigned int n_line)
{
	struct bio *result;
	cache_line_t *line;
	CHECK_ARG_RET_NULL(cache);
	CHECK_PLAIN_RET_NULL(n_line < cache->n_lines);

	line = cache->lines[n_line];
	CHECK_PLAIN_RET_NULL(line);

	result = line->waiting;
	if (result) line->waiting = result->bi_next;

	return result;
}

static unsigned int raidxor_cache_line_length_requests(cache_t *cache,
						       unsigned int n_line)
{
	unsigned int result = 0;
	struct bio *bio = cache->lines[n_line]->waiting;

	while (bio) {
		bio = bio->bi_next;
		++result;
	}

	return result;
}

/**
 * raidxor_find_bio() - searches for the corresponding bio for a single unit
 *
 * Returns NULL if not found.
 */
static struct bio * raidxor_find_bio(raidxor_bio_t *rxbio, disk_info_t *unit)
{
	unsigned int i;

	CHECK_ARG_RET_NULL(rxbio);
	CHECK_ARG_RET_NULL(unit);

	/* goes to the bdevs to find a matching bio */
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]->bi_bdev == unit->rdev->bdev)
			return rxbio->bios[i];
	return NULL;
}

static int raidxor_find_enc_temps(raidxor_conf_t *conf, encoding_t *temp)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	unsigned int i;

	CHECK_ARG_RET_VAL(conf);
	CHECK_ARG_RET_VAL(temp);

	for (i = 0; i < conf->n_enc_temps; ++i)
		if (conf->enc_temps[i] == temp)
			return i;
	return 0;
}

static int raidxor_find_dec_temps(raidxor_conf_t *conf, decoding_t *temp)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	unsigned int i;

	CHECK_ARG_RET_VAL(conf);
	CHECK_ARG_RET_VAL(temp);

	for (i = 0; i < conf->n_dec_temps; ++i)
		if (conf->dec_temps[i] == temp)
			return i;
	return 0;
}

static disk_info_t * raidxor_find_unit_conf_rdev(raidxor_conf_t *conf,
						 mdk_rdev_t *rdev)
{
	unsigned int i;

	CHECK_ARG_RET_NULL(conf);
	CHECK_ARG_RET_NULL(rdev);

	for (i = 0; i < conf->n_units; ++i)
		if (conf->units[i].rdev == rdev)
			return &conf->units[i];
	return NULL;
}

static disk_info_t * raidxor_find_unit_decoding(decoding_t *decoding,
						disk_info_t *unit)
{
	unsigned int i;

	CHECK_ARG_RET_NULL(decoding);
	CHECK_ARG_RET_NULL(unit);

	for (i = 0; i < decoding->n_units; ++i)
		if (decoding->units[i].disk == unit)
			return unit;

	return NULL;
}

static unsigned int raidxor_unit_to_index(raidxor_conf_t *conf,
					  disk_info_t *unit)
{
	unsigned int i;

	for (i = 0; i < conf->n_units; ++i)
		if (&conf->units[i] == unit)
			return i;

	CHECK_BUG("didn't find unit, badbadbad");
	return 0;
}

static void clear_bio(struct bio *bio)
{
	unsigned int i;

	CHECK_ARG_RET(bio);

	for (i = 0; i < bio->bi_vcnt; ++i)
		bio->bi_io_vec[i].bv_page = NULL;
}

/**
 * free_bio() - puts all pages in a bio and the bio itself
 */
static void free_bio(struct bio *bio)
{
	unsigned int i;

	CHECK_ARG_RET(bio);

	for (i = 0; i < bio->bi_vcnt; ++i)
		safe_put_page(bio->bi_io_vec[i].bv_page);
	bio_put(bio);
}

/**
 * clear_bios() - 
 */
static void clear_bios(raidxor_bio_t *rxbio)
{
	unsigned int i;

	CHECK_ARG_RET(rxbio);

	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]) clear_bio(rxbio->bios[i]);
}

/**
 * free_bios() - puts all bios (and their pages) in a raidxor_bio_t
 */
static void free_bios(raidxor_bio_t *rxbio)
{
	unsigned int i;

	CHECK_ARG_RET(rxbio);

	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]) {
			free_bio(rxbio->bios[i]);
			rxbio->bios[i] = NULL;
		}
}

static raidxor_bio_t * raidxor_alloc_bio(unsigned int nbios)
{
	raidxor_bio_t *result;

	CHECK_PLAIN_RET_NULL(nbios);

	result = kzalloc(sizeof(raidxor_bio_t) +
			 sizeof(struct bio *) * nbios,
			 GFP_NOIO);
	CHECK_ALLOC_RET_NULL(result);

	result->n_bios = nbios;
	return result;
}

static void raidxor_free_bio(raidxor_bio_t *rxbio)
{
	clear_bios(rxbio);
	free_bios(rxbio);
	kfree(rxbio);
}

static void raidxor_cache_line_free_temps(cache_t *cache, unsigned int line)
{
	unsigned int i;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(line < cache->n_lines);

	if (!cache->lines[line]->temp_buffers)
		return;

	for (i = 0; i < max(cache->conf->n_enc_temps, cache->conf->n_dec_temps) * cache->n_chunk_mult; ++i) {
		safe_put_page(cache->lines[line]->temp_buffers[i]);
		cache->lines[line]->temp_buffers[i] = NULL;
	}

	kfree(cache->lines[line]->temp_buffers);
	cache->lines[line]->temp_buffers = NULL;
}

static void raidxor_cache_free_temps(cache_t *cache)
{
	unsigned int i;

	CHECK_ARG_RET(cache);

	for (i = 0; i < cache->n_lines; ++i)
		raidxor_cache_line_free_temps(cache, i);
}

static void raidxor_cache_drop_line(cache_t *cache, unsigned int line)
{
	unsigned int i;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(line < cache->n_lines);

	for (i = 0; i < (cache->n_buffers + cache->n_red_buffers) * cache->n_chunk_mult; ++i) {
		safe_put_page(cache->lines[line]->buffers[i]);
		cache->lines[line]->buffers[i] = NULL;
	}

	raidxor_cache_line_free_temps(cache, line);
}

/**
 * raidxor_alloc_cache() - allocates a new cache with buffers
 * @n_lines: number of available lines in the cache
 * @n_buffers: buffers per line (equivalent to the width of the stripe times
               chunk_mult)
 * @n_red_buffers: redundant buffers per line
 * @n_chunk_mult: number of buffers per chunk
 */
static cache_t * raidxor_alloc_cache(unsigned int n_lines,
				     unsigned int n_buffers,
				     unsigned int n_red_buffers,
				     unsigned int n_chunk_mult)
{
	unsigned int i;
	cache_t *cache;

	CHECK_PLAIN_RET_NULL(n_lines != 0);
	CHECK_PLAIN_RET_NULL(n_buffers != 0);
	CHECK_PLAIN_RET_NULL(n_chunk_mult != 0);

	cache = kzalloc(sizeof(cache_t) + sizeof(cache_line_t *) * n_lines,
			GFP_NOIO);
	CHECK_ALLOC_RET_NULL(cache);

	for (i = 0; i < n_lines; ++i) {
		cache->lines[i] = kzalloc(sizeof(cache_line_t) +
					  sizeof(struct page *) *
					  (n_buffers + n_red_buffers) *
					  n_chunk_mult,
					  GFP_NOIO);
		if (!cache->lines[i])
			goto out_free_lines;
		cache->lines[i]->status = CACHE_LINE_CLEAN;
	}

	cache->n_lines = n_lines;
	cache->n_buffers = n_buffers;
	cache->n_red_buffers = n_red_buffers;
	cache->n_chunk_mult = n_chunk_mult;
	cache->n_waiting = 0;

	init_waitqueue_head(&cache->wait_for_line);

	return cache;

out_free_lines:
	for (i = 0; i < cache->n_lines; ++i)
		if (cache->lines[i]) kfree(cache->lines[i]);
	kfree(cache);
	return NULL;
}

static int raidxor_cache_line_ensure_temps(cache_t *cache, unsigned int line)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i;
	unsigned int to = max(cache->conf->n_enc_temps,
			      cache->conf->n_dec_temps) *
		cache->n_chunk_mult;

	CHECK_ARG(cache);
	CHECK_PLAIN(line < cache->n_lines);

	if (cache->lines[line]->temp_buffers)
		return 0;

	cache->lines[line]->temp_buffers = kzalloc(sizeof(struct page *) * to,
						   GFP_NOIO);
	if (!cache->lines[line]->temp_buffers)
		goto out;

	for (i = 0; i < to; ++i) {
		if (!(cache->lines[line]->temp_buffers[i] = alloc_page(GFP_NOIO))) {
			printk(KERN_INFO "page allocation failed for line %u\n", line);
			goto out_free_pages;
		}

		raidxor_fill_page(cache->lines[line]->temp_buffers[i], 0, PAGE_SIZE);
	}

	return 0;
out_free_pages:
	raidxor_cache_line_free_temps(cache, line);
out:
	return 1;
}

static void raidxor_free_cache(cache_t *cache)
{
	unsigned int i;

	CHECK_ARG_RET(cache);

	for (i = 0; i < cache->n_lines; ++i) {
		raidxor_cache_drop_line(cache, i);
		kfree(cache->lines[i]);
	}

	kfree(cache);
}

static void raidxor_cache_abort_requests(cache_t *cache, unsigned int line)
{
	struct bio *bio;

	while ((bio = raidxor_cache_remove_request(cache, line)))
		bio_io_error(bio);
}

static void raidxor_safe_free_decoding(disk_info_t *unit)
{
	if (unit->decoding) {
		kfree(unit->decoding);
		unit->decoding = NULL;
	}
}

static void raidxor_safe_free_encoding(disk_info_t *unit)
{
	if (unit->decoding) {
		kfree(unit->encoding);
		unit->encoding = NULL;
	}
}

static int raidxor_alloc_enc_temps(raidxor_conf_t *conf)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i;

	CHECK_FUN(raidxor_alloc_enc_temps);

	CHECK_ARG(conf);

	if (conf->enc_temps)
		return 0;

	conf->enc_temps = kzalloc(sizeof(encoding_t *) * conf->n_enc_temps,
				  GFP_NOIO);
	if (!conf->enc_temps)
		goto out;

	return 0;
out:
	return 1;
}

static int raidxor_alloc_dec_temps(raidxor_conf_t *conf)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i;

	CHECK_FUN(raidxor_alloc_dec_temps);

	CHECK_ARG(conf);

	if (conf->dec_temps)
		return 0;

	conf->dec_temps = kzalloc(sizeof(decoding_t *) * conf->n_dec_temps,
				  GFP_NOIO);
	CHECK_PLAIN(conf->dec_temps);

	return 0;
out:
	return 1;
}

static void raidxor_safe_free_enc_temps(raidxor_conf_t *conf)
{
	unsigned int i;

	CHECK_ARG_RET(conf);

	if (!conf->enc_temps)
		return;

	for (i = 0; i < conf->n_enc_temps; ++i)
		if (conf->enc_temps[i]) kfree(conf->enc_temps[i]);

	kfree(conf->enc_temps);
	conf->enc_temps = NULL;
}

static void raidxor_safe_free_dec_temps(raidxor_conf_t *conf)
{
	unsigned int i;

	CHECK_ARG_RET(conf);

	if (!conf->dec_temps)
		return;

	for (i = 0; i < conf->n_dec_temps; ++i)
		if (conf->dec_temps[i]) kfree(conf->dec_temps[i]);

	kfree(conf->dec_temps);
	conf->dec_temps = NULL;
}

static int raidxor_ensure_enc_temps(raidxor_conf_t *conf,
				    unsigned int ntemps)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(conf);

	if (ntemps == conf->n_enc_temps)
		return 0;

	raidxor_safe_free_enc_temps(conf);

	if (ntemps == 0)
		return 0;

	conf->n_enc_temps = ntemps;

	return raidxor_alloc_enc_temps(conf);
}

static int raidxor_ensure_dec_temps(raidxor_conf_t *conf,
				    unsigned int ntemps)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 1
	CHECK_ARG_RET_VAL(conf);

	if (ntemps == conf->n_dec_temps)
		return 0;

	raidxor_safe_free_dec_temps(conf);

	if (ntemps == 0)
		return 0;

	conf->n_dec_temps = ntemps;

	return raidxor_alloc_dec_temps(conf);
}

static int raidxor_cache_ensure_temps(raidxor_conf_t *conf, unsigned int n_enc_temps, unsigned n_dec_temps)
{
#undef CHECK_JUMP_LABEL
#define CHECK_JUMP_LABEL out
	unsigned int i;

	CHECK_ARG(conf);

	if (n_enc_temps == conf->n_enc_temps &&
	    n_dec_temps == conf->n_dec_temps)
		return 0;

	raidxor_ensure_dec_temps(conf, n_dec_temps);
	raidxor_ensure_enc_temps(conf, n_enc_temps);

	if (!conf->cache)
		return 0;

	raidxor_cache_free_temps(conf->cache);

	for (i = 0; i < conf->cache->n_lines; ++i)
		if (raidxor_cache_line_ensure_temps(conf->cache, i))
			goto out_free_pages;

	return 0;
out_free_pages:
	raidxor_cache_free_temps(conf->cache);
out:
	return 1;
}


/**
 * raidxor_safe_free_conf() - frees resource information
 *
 * Must be called inside conf lock.
 */
static void raidxor_safe_free_conf(raidxor_conf_t *conf)
{
	unsigned int i;

	CHECK_ARG_RET(conf);

	set_bit(CONF_INCOMPLETE, &conf->flags);

	if (conf->resources != NULL) {
		for (i = 0; i < conf->n_resources; ++i)
			kfree(conf->resources[i]);
		kfree(conf->resources);
		conf->resources = NULL;
	}

	if (conf->cache != NULL) {
		raidxor_free_cache(conf->cache);
		conf->cache = NULL;
	}
}

static void raidxor_complete_free_conf(raidxor_conf_t *conf)
{
	unsigned int i;

	CHECK_ARG_RET(conf);

	raidxor_safe_free_enc_temps(conf);
	raidxor_safe_free_dec_temps(conf);

	for (i = 0; i < conf->n_units; ++i) {
		raidxor_safe_free_encoding(&conf->units[i]);
		raidxor_safe_free_decoding(&conf->units[i]);
	}
}

/**
 * raidxor_copy_bio_to_cache() - copies data from a bio to cache
 */
static void raidxor_copy_bio_to_cache(cache_t *cache, unsigned int n_line,
				      struct bio *bio)
{
	/* bio->bi_sector is the offset into the line.
	   every bio->bi_io_vec should be of size PAGE_SIZE */
	struct bio_vec *bvl;
	unsigned int i, j;
	char *bio_mapped, *page_mapped;
	sector_t offset;
	cache_line_t *line;

	CHECK_FUN(raidxor_copy_bio_to_cache);

	offset = bio->bi_sector;
	line = cache->lines[n_line];

	/* skip offset / some pages */
	j = 0;
	while (offset > 0) {
		offset -= PAGE_SIZE >> 9;
		++j;
	}

	CHECK_PLAIN_RET(offset >= 0);

	bio_for_each_segment(bvl, bio, i) {
		bio_mapped = __bio_kmap_atomic(bio, i, KM_USER0);
		//page_mapped = kmap_atomic(line->buffers[j], KM_USER0);
		page_mapped = kmap(line->buffers[j]);

		memmove(page_mapped, bio_mapped, PAGE_SIZE);

		//kunmap_atomic(page_mapped, KM_USER0);
		kunmap(line->buffers[j]);
		__bio_kunmap_atomic(bio_mapped, KM_USER0);
		++j;
	}
}

/**
 * raidxor_copy_bio_from_cache() - copies data from cache to a bio
 */
static void raidxor_copy_bio_from_cache(cache_t *cache, unsigned int n_line,
					struct bio *bio)
{
	/* bio->bi_sector is the offset into the line.
	   every bio->bi_io_vec should be of size PAGE_SIZE */
	struct bio_vec *bvl;
	unsigned int i, j;
	char *bio_mapped, *page_mapped;
	sector_t offset;
	cache_line_t *line;

	CHECK_FUN(raidxor_copy_bio_from_cache);

	offset = bio->bi_sector;
	line = cache->lines[n_line];

	/* skip offset / some pages */
	j = 0;
	while (offset > 0) {
		offset -= PAGE_SIZE >> 9;
		++j;
	}

	CHECK_PLAIN_RET(offset >= 0);

	bio_for_each_segment(bvl, bio, i) {
		bio_mapped = __bio_kmap_atomic(bio, i, KM_USER0);
		//page_mapped = kmap_atomic(line->buffers[j], KM_USER0);
		page_mapped = kmap(line->buffers[j]);

		/* printk(KERN_EMERG "copying %lu bytes for index %d, buffer %d\n", PAGE_SIZE, i, j); */

		memmove(bio_mapped, page_mapped, PAGE_SIZE);

		//kunmap_atomic(page_mapped, KM_USER0);
		kunmap(line->buffers[j]);
		__bio_kunmap_atomic(bio_mapped, KM_USER0);
		++j;
	}
}

static void raidxor_copy_bio(struct bio *bioto, struct bio *biofrom)
{
	unsigned int i;
	unsigned char *tomapped, *frommapped;
	unsigned char *toptr, *fromptr;
	struct bio_vec *bvto, *bvfrom;

	for (i = 0; i < bioto->bi_vcnt; ++i) {
		bvto = bio_iovec_idx(bioto, i);
		bvfrom = bio_iovec_idx(biofrom, i);

		tomapped = (unsigned char *) kmap(bvto->bv_page);
		frommapped = (unsigned char *) kmap(bvfrom->bv_page);

		toptr = tomapped + bvto->bv_offset;
		fromptr = frommapped + bvfrom->bv_offset;

		memmove(toptr, fromptr, min(bvto->bv_len, bvfrom->bv_len));

		kunmap(bvfrom->bv_page);
		kunmap(bvto->bv_page);
	}
}

static void raidxor_copy_pages_to_bio(struct bio *bioto, struct page **pages)
{
	unsigned int i;
	unsigned char *tomapped, *frommapped;
	struct bio_vec *bvto;

	for (i = 0; i < bioto->bi_vcnt; ++i) {
		bvto = bio_iovec_idx(bioto, i);

		tomapped = (unsigned char *) kmap(bvto->bv_page) + bvto->bv_offset;
		frommapped = (unsigned char *) kmap(pages[i]);

		memmove(tomapped, frommapped, bvto->bv_len);

		kunmap(pages[i]);
		kunmap(bvto->bv_page);
	}
}

static void raidxor_copy_bio_to_pages(struct page **pages, struct bio *biofrom)
{
	unsigned int i;
	unsigned char *tomapped, *frommapped;
	struct bio_vec *bvfrom;

	for (i = 0; i < biofrom->bi_vcnt; ++i) {
		bvfrom = bio_iovec_idx(biofrom, i);

		tomapped = (unsigned char *) kmap(pages[i]);
		frommapped = (unsigned char *) kmap(bvfrom->bv_page) + bvfrom->bv_offset;

		memmove(tomapped, frommapped, bvfrom->bv_len);

		kunmap(pages[i]);
		kunmap(bvfrom->bv_page);
	}
}

static void raidxor_copy_pages(unsigned int length, struct page **to, struct page **from)
{
	unsigned int i;
	unsigned char *tomapped, *frommapped;

	for (i = 0; i < length; ++i) {
		tomapped = (unsigned char *) kmap(to[i]);
		frommapped = (unsigned char *) kmap(from[i]);

		memmove(tomapped, frommapped, PAGE_SIZE);

		kunmap(from[i]);
		kunmap(to[i]);
	}
}

/**
 * raidxor_cache_find_line() - finds a matching or otherwise available line
 *
 * Returns 1 if we've found a line, else 0.
 */
static int raidxor_cache_find_line(cache_t *cache, sector_t sector,
				   unsigned int *line)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	unsigned int i;

	CHECK_ARG_RET_VAL(cache);

	/* find an exact match */
	for (i = 0; i < cache->n_lines; ++i) {
		if (sector == cache->lines[i]->sector) {
			if (line) *line = i;
			return 1;
		}
	}

	/* find lines to reassign */
	for (i = 0; i < cache->n_lines; ++i) {
		if (cache->lines[i]->status == CACHE_LINE_CLEAN) {
			if (line) *line = i;
			return 1;
		}
		else if (cache->lines[i]->status == CACHE_LINE_READY &&
			 !cache->lines[i]->waiting) {
			if (line) *line = i;
			return 1;
		}
	}

	return 0;
}

static unsigned int raidxor_cache_empty_lines(cache_t *cache)
{
#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	unsigned int i;
	unsigned result = 0;

	CHECK_ARG_RET_VAL(cache);

	for (i = 0; i < cache->n_lines; ++i)
		if (cache->lines[i]->status == CACHE_LINE_CLEAN ||
		    cache->lines[i]->status == CACHE_LINE_READY)
			++result;

	return result;
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

#define __wait_event_lock_irqsave(wq, condition, lock, flags, cmd) 	\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_UNINTERRUPTIBLE);		\
		if (condition)						\
			break;						\
		spin_unlock_irqrestore(&lock, flags);			\
		cmd;							\
		schedule();						\
		spin_lock_irqsave(&lock, flags);			\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)

#define wait_event_lock_irqsave(wq, condition, lock, flags, cmd) 	\
do {									\
	if (condition)	 						\
		break;							\
	__wait_event_lock_irqsave(wq, condition, lock, flags, cmd);	\
} while (0)

/**
 * raidxor_wait_for_no_active_lines() - waits until no lines are active
 *
 * Needs to be called with conf->device_lock held.
 */
static void raidxor_wait_for_no_active_lines(raidxor_conf_t *conf, unsigned long *flags)
{
	CHECK_ARG_RET(conf);

	#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "active_lines = %d\n", conf->cache->active_lines);
	printk(KERN_INFO "WAITING\n");
	#endif

	wait_event_lock_irqsave(conf->cache->wait_for_line,
				conf->cache->active_lines == 0,
				conf->device_lock, *flags, /* nothing */);
	#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "WAITING DONE\n");
	#endif
}

/**
 * raidxor_wait_for_empty_line() - blocks until a cache line is available
 *
 * Needs to be called with conf->device_lock held.
 *
 * Also stops if CONF_STOPPING is set (but you've to test for that
 * condition nevertheless.
 */
static void raidxor_wait_for_empty_line(raidxor_conf_t *conf, unsigned long *flags)
{
	CHECK_ARG_RET(conf);

	/* signal raidxord to free some lines */
	++conf->cache->n_waiting;
	raidxor_wakeup_thread(conf);

	#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "WAITING\n");
	#endif

	wait_event_lock_irqsave(conf->cache->wait_for_line,
				raidxor_cache_empty_lines(conf->cache) > 0 ||
				test_bit(CONF_STOPPING, &conf->flags),
				conf->device_lock, *flags,
#ifdef RAIDXOR_DEBUG
				printk(KERN_INFO "wait condition still not matched: %d, still waiting %d\n",
				       raidxor_cache_empty_lines(conf->cache),
				       conf->cache->n_waiting)
#endif
);

	#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "WAITING DONE\n");
	#endif
	--conf->cache->n_waiting;
}

static void raidxor_wait_for_writeback(raidxor_conf_t *conf, unsigned long *flags)
{
	CHECK_ARG_RET(conf);

	/* signal raidxord to free all lines */
	conf->cache->n_waiting = conf->cache->n_lines;
	raidxor_wakeup_thread(conf);

#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "active_lines = %d, n_waiting = %d\n",
	       conf->cache->active_lines,
	       conf->cache->n_waiting);
	printk(KERN_INFO "WAITING\n");
#endif

	wait_event_lock_irqsave(conf->cache->wait_for_line,
				raidxor_cache_empty_lines(conf->cache) == conf->cache->n_lines ||
				(conf->cache->active_lines == 0 &&
				 conf->cache->n_waiting == 0),
				conf->device_lock, *flags, /* nothing */);
#ifdef RAIDXOR_DEBUG
	printk(KERN_INFO "WAITING DONE\n");
#endif
}

/**
 * raidxor_signal_empty_line() - signals the cache line available signal
 *
 * Needs to be called with conf->device_lock held.
 */
static void raidxor_signal_empty_line(raidxor_conf_t *conf)
{
	CHECK_ARG_RET(conf);

	wake_up(&conf->cache->wait_for_line);
}

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
