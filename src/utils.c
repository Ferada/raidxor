/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

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

	line = &cache->lines[n_line];
	CHECK_PLAIN_RET(line);

	previous = line->waiting;
	if (!previous) {
		line->waiting = bio;
	}
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

	line = &cache->lines[n_line];
	CHECK_PLAIN_RET_NULL(line);

	result = line->waiting;
	if (result) {
		line->waiting = result->bi_next;
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
	unsigned long i;

	CHECK_ARG_RET_NULL(rxbio);
	CHECK_ARG_RET_NULL(unit);

	/* goes to the bdevs to find a matching bio */
	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]->bi_bdev == unit->rdev->bdev)
			return rxbio->bios[i];
	return NULL;
}

/**
 * raidxor_find_unit() - searches the corresponding unit for a rdev
 *
 * Returns NULL if not found.
 */
static disk_info_t * raidxor_find_unit_bdev(stripe_t *stripe,
					    struct block_device *bdev)
{
	unsigned long i;

	CHECK_ARG_RET_NULL(stripe);
	CHECK_ARG_RET_NULL(bdev);

	for (i = 0; i < stripe->n_units; ++i)
		if (stripe->units[i]->rdev->bdev == bdev)
			return stripe->units[i];
	return NULL;
}

/**
 * raidxor_find_unit() - searches the corresponding unit for a rdev
 *
 * Returns NULL if not found.
 */
static disk_info_t * raidxor_find_unit_rdev(stripe_t *stripe, mdk_rdev_t *rdev)
{
	return raidxor_find_unit_bdev(stripe, rdev->bdev);
}

/**
 * raidxor_find_unit() - searches the corresponding unit for a single bio
 *
 * Returns NULL if not found.
 */
static disk_info_t * raidxor_find_unit_bio(stripe_t *stripe, struct bio *bio)
{
	return raidxor_find_unit_bdev(stripe, bio->bi_bdev);
}

static disk_info_t * raidxor_find_unit_conf_rdev(raidxor_conf_t *conf, mdk_rdev_t *rdev)
{
	unsigned int i;

	CHECK_ARG_RET_NULL(conf);
	CHECK_ARG_RET_NULL(rdev);

	for (i = 0; i < conf->n_units; ++i)
		if (conf->units[i].rdev == rdev)
			return &conf->units[i];
	return NULL;
}

/**
 * free_bio() - puts all pages in a bio and the bio itself
 */
static void free_bio(struct bio *bio)
{
	unsigned long i;

	CHECK_ARG_RET(bio);

	for (i = 0; i < bio->bi_vcnt; ++i)
		safe_put_page(bio->bi_io_vec[i].bv_page);
	bio_put(bio);
}

/**
 * free_bios() - puts all bios (and their pages) in a raidxor_bio_t
 */
static void free_bios(raidxor_bio_t *rxbio)
{
	unsigned long i;

	CHECK_ARG_RET(rxbio);

	for (i = 0; i < rxbio->n_bios; ++i)
		if (rxbio->bios[i]) free_bio(rxbio->bios[i]);
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
	free_bios(rxbio);
	kfree(rxbio);
}

/**
 * raidxor_alloc_cache() - allocates a new cache with buffers
 * @n_lines: number of available lines in the cache
 * @n_buffers: buffers per line (equivalent to the width of a stripe times
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

	cache = kzalloc(sizeof(cache_t) +
			(sizeof(cache_line_t) +
			 sizeof(struct page *) * (n_buffers + n_red_buffers)) * n_lines,
			GFP_NOIO);
	CHECK_ALLOC_RET_NULL(cache);

	cache->n_lines = n_lines;
	cache->n_buffers = n_buffers;
	cache->n_red_buffers = n_red_buffers;
	cache->n_chunk_mult = n_chunk_mult;
	cache->n_waiting = 0;

	init_waitqueue_head(&cache->wait_for_line);

	for (i = 0; i < n_lines; ++i)
		cache->lines[i].status = CACHE_LINE_CLEAN;
	return cache;
}

static void raidxor_cache_abort_requests(cache_t *cache, unsigned int line)
{
	struct bio *bio;

	while ((bio = raidxor_cache_remove_request(cache, line))) {
		bio_io_error(bio);
	}
}

static void raidxor_cache_drop_line(cache_t *cache, unsigned int line)
{
	unsigned int i;

	CHECK_ARG_RET(cache);
	CHECK_PLAIN_RET(line < cache->n_lines);

	for (i = 0; i < cache->n_buffers + cache->n_red_buffers; ++i)
		safe_put_page(cache->lines[line].buffers[i]);
}

static void raidxor_free_cache(cache_t *cache)
{
	unsigned int i;

	CHECK_ARG_RET(cache);

	for (i = 0; i < cache->n_lines; ++i)
		raidxor_cache_drop_line(cache, i);

	kfree(cache);
}

/**
 * raidxor_safe_free_conf() - frees resource and stripe information
 *
 * Must be called inside conf lock.
 */
static void raidxor_safe_free_conf(raidxor_conf_t *conf) {
	unsigned long i;

	CHECK_ARG_RET(conf);

	conf->configured = 0;

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

	raidxor_free_cache(conf->cache);

	kfree(conf);
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

	offset = bio->bi_sector;
	line = &cache->lines[n_line];

	/* skip offset / some pages */
	j = 0;
	while (offset > 0) {
		offset -= PAGE_SIZE >> 9;
		++j;
	}

	bio_for_each_segment(bvl, bio, i) {
		bio_mapped = __bio_kmap_atomic(bio, i, KM_USER0);
		page_mapped = kmap_atomic(line->buffers[j], KM_USER0);

		memcpy(page_mapped, bio_mapped, PAGE_SIZE);

		kunmap_atomic(line->buffers[j], KM_USER0);
		__bio_kunmap_atomic(bio_mapped, KM_USER0);
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

	offset = bio->bi_sector;
	line = &cache->lines[n_line];

	/* skip offset / some pages */
	j = 0;
	while (offset > 0) {
		offset -= PAGE_SIZE >> 9;
		++j;
	}

	bio_for_each_segment(bvl, bio, i) {
		bio_mapped = __bio_kmap_atomic(bio, i, KM_USER0);
		page_mapped = kmap_atomic(line->buffers[j], KM_USER0);

		memcpy(bio_mapped, page_mapped, PAGE_SIZE);

		kunmap_atomic(line->buffers[j], KM_USER0);
		__bio_kunmap_atomic(bio_mapped, KM_USER0);
	}
}

static void raidxor_copy_bio(struct bio *bioto, struct bio *biofrom)
{
	unsigned long i;
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

		memcpy(toptr, fromptr, min(bvto->bv_len, bvfrom->bv_len));

		kunmap(bvfrom->bv_page);
		kunmap(bvto->bv_page);
	}
}

/**
 * raidxor_sector_to_stripe() - returns the stripe a virtual sector belongs to
 * @newsector: if non-NULL, the sector in the stripe is written here
 */
static stripe_t * raidxor_sector_to_stripe(raidxor_conf_t *conf, sector_t sector,
					   sector_t *newsector)
{
	unsigned int sectors_per_chunk = conf->chunk_size >> 9;
	stripe_t **stripes = conf->stripes;
	unsigned long i;

	printk(KERN_EMERG "raidxor: sectors_per_chunk %u\n",
	       sectors_per_chunk);

	for (i = 0; i < conf->n_stripes; ++i) {
		printk(KERN_EMERG "raidxor: stripe %lu, sector %lu\n",
		       i, (unsigned long) sector);
		if (sector <= stripes[i]->size >> 9)
			break;
		sector -= stripes[i]->size >> 9;
	}

	if (newsector)
		*newsector = sector;

	return stripes[i];
}

static int raidxor_bio_maybe_split_boundary(stripe_t *stripe, struct bio *bio,
					    sector_t newsector,
					    struct bio_pair **split)
{
	unsigned long result = stripe->size - (newsector << 9);
	if (result < bio->bi_size) {
		/* FIXME: what should first_sectors really be? */
		//*split = bio_split(bio, NULL, result);
		*split = NULL;
		return 1;
	}
	return 0;
}

/**
 * raidxor_cache_find_line() - finds a matching or otherwise available line
 *
 * Returns 1 if we've found a line, else 0.
 */
static int raidxor_cache_find_line(cache_t *cache, sector_t sector,
				   unsigned int *line)
{
	unsigned int i;

#undef CHECK_RETURN_VALUE
#define CHECK_RETURN_VALUE 0
	CHECK_ARG_RET_VAL(cache);

	/* find an exact match */
	for (i = 0; i < cache->n_lines; ++i) {
		if (sector == cache->lines[i].sector) {
			if (line)
				*line = i;
			return 1;
		}
	}

	for (i = 0; i < cache->n_lines; ++i) {
		if (cache->lines[i].status == CACHE_LINE_CLEAN)
			return i;
		else if (cache->lines[i].status == CACHE_LINE_READY &&
			 !cache->lines[i].waiting)
			return i;
	}

	return 0;
}

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
