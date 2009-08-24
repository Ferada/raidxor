#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)



typedef struct disk_info disk_info_t;
typedef struct encoding encoding_t;
typedef struct raidxor_bio raidxor_bio_t;
typedef struct raidxor_conf raidxor_conf_t;
typedef struct stripe stripe_t;
typedef struct raidxor_resource resource_t;
typedef struct cache cache_t;
typedef struct cache_line cache_line_t;
typedef struct raidxor_request raidxor_request_t;


/**
 * struct cache_line - buffers multiple blocks over a stripe
 * @flags: current status of the line
 * @sector: index into the virtual device
 * @waiting: waiting requests
 * @buffers: actual data
 */
struct cache_line {
	unsigned int flags;
	sector_t sector;

	struct bio *waiting;

	struct page *buffers[0];
};



/**
 * struct cache - groups access to the individual cache lines
 * @n_lines: number of lines
 * @n_buffers: number of actual buffers in each line
 * @n_chunk_mult: number of buffers per chunk
 *
 * device_lock needs to be hold when accessing the cache.
 */
struct cache {
	raidxor_conf_t *conf;
	atomic_t active_lines;
	unsigned int n_lines, n_buffers, n_chunk_mult;

	cache_line_t lines[0];
};

#define CACHE_LINE_CLEAN     0
#define CACHE_LINE_READY     1
#define CACHE_LINE_LOADING   2
#define CACHE_LINE_UPTODATE  3
#define CACHE_LINE_DIRTY     4
#define CACHE_LINE_WRITEBACK 5
#define CACHE_LINE_ERROR     6

static cache_t * raidxor_alloc_cache(unsigned int n_lines, unsigned int n_buffers,
				     unsigned int n_chunk_mult);

/*
   Life cycle of a cache line:

   +=======+ 1 	+-------+ 2  +---------+
   ¦ CLEAN ¦<==>| READY |<==>| LOADING |
   +=======+  	+-------+    +---------+
		  ^ 	   	---
		  | 	-------/
		 4|    /   3	      +-------+
                  |   v		      | ERROR |
                +----------+	      +-------+
                | UPTODATE |<-\ 7	  ^ 
                +----------+   ----\	 8|
         	 5 |	   	    \+----------+
		   v	 	  /->| WRITEBACK|
		+----------+  /---   +----------+
		|  DIRTY   |--  6
		+----------+

   1: allocating pages or dropping them
   2: loading data from disk
   3: finishing read from disk
   4: dropping a cache line
   5: write some data to the cache
   6: start writeback process
   7: finishing writeback, data in cache and on device is now synchronised
   8: we couldn't finish writeback, just leave the data here

   requests are limited to multiple of PAGE_SIZE bytes, so all we have to do,
   is to take these requests, scatter their data into the cache, and write
   that back to disk (or load from there)

   with blk_queue_max_sectors the maximum number of bytes is set to one
   cache line, we just have to care for smaller requests

   if the cache is full, some entries have to go.

   currently loading or backwriting entries can not be touched.
   we prefer ready ones first, then clean, then uptodate, then dirty.
   dirty needs a writeback, so we have to start that and see later, if
   we can do something.  how do we support waiting?
   raid5.c uses wait_event_lock_irq for something like that.
   we could also abort the request in the meantime ...

   if we encounter an error, we stay in the state and start the recovery
   routine, which waits for decoding equations and rebuilds the missing
   data in the already read buffers.  the error callee needs to provide
   the broken device, so that we can start the correct recovery equation.

   if recovery is successful, we end up in the correct state.  globally, we
   record, which device failed (which is considered broken forever).
   if its unable to recover, we go back to ready and completely abort the
   request

   recovery is handled inside the bio layer, the necessary generic_requests
   are started inline (outside of raidxord).

   since an error during writeback is very bad, we just stop there, go to
   the error state, which is not purged from the cache.

   if the whole cache is full of error lines, we naturally bring the whole
   raid to read-only mode and abort request to sectors other than already
   in cache.

   void raid_abort_readonly();
     - sets abort flag
     - waits until all operations are done?
   void cache_line_recover(line);
     - looks for equations
     - checks if recovery is even possible
     - loads blocks from the redundant devices
     - merges them
     - commits changes to the cache
     - completes request? (callback?)
 */


/**
 * struct disk_info - a unit in the raid, either data or redundancy disk
 * @rdev: the md level device information
 * @redundant: contains 1 if it is a redundant unit, 0 if not, -1 if
 *             uninitialized
 * @encoding: contains the encoding equation if it's a redundant unit
 * @resource: the resource this unit belongs to
 * @stripe: the stripe this unit belongs to
 *
 * This is the smallest building block in this driver.  One unit is the
 * actual backing storage for data, either redundancy information or
 * actual raw data.
 *
 * See struct resource.
 */
struct disk_info {
	mdk_rdev_t *rdev;

	int redundant;
	encoding_t *encoding;

	resource_t *resource;
	stripe_t *stripe;
};



/**
 * struct encoding - encoding information for a unit
 * @n_units: the number of contained units
 * @units: the actual units
 *
 * Because we might change the encoding information at some time,
 * this structure encapsulates all information necessary to encode
 * data for a unit.  Chunks from the units are XORed together to
 * compute the redundancy information.
 */
struct encoding {
	unsigned int n_units;
	disk_info_t *units[0];
};



/**
 * struct resource - one resource consists of many units
 * @n_units: the number of contained units
 * @units: the actual units
 *
 * In the rectangular raid layout, this is a row of units.
 */
struct raidxor_resource {
	unsigned int n_units;
	disk_info_t *units[0];
};



/**
 * struct stripe - one stripe over multiple resources and units
 * @size: size in sectors, that is, 1024 bytes
 * @n_units: the number of contained units
 * @units: the actual units
 *
 * In the rectangular raid layout, this is a column of units.
 *
 * This is the correct level for en- and decoding.
 *
 * units = [u1 r1 r2 u2 u3] order as in parameter list.
 */
struct stripe {
	sector_t size;
	unsigned int n_data_units;
	unsigned int n_units;
	disk_info_t *units[0];
};

static stripe_t * raidxor_sector_to_stripe(raidxor_conf_t *conf,
					   sector_t sector,
					   sector_t *newsector);


/**
 * struct raidxor_private_data_s - private data per mddev
 * @mddev: the mddev we are associated to
 * @device_lock: lock for exclusive access to this raid
 * @status: one of RAIDXOR_CONF_STATUS_{NORMAL,INCOMPLETE,ERROR}
 * @chunk_size: copied from mddev_t, in bytes
 * @waiting_list: requests to be queued into the cache
 * @configured: is 1 if we have all necessary information
 * @units_per_resource: the number of units per resource
 * @n_resources: the number of resources
 * @resources: the actual resources
 * @n_stripes: the number of stripes
 *
 * Since we have no easy way to get additional information, we postpone it
 * after raidxor_run and return errors until we have configured the raid.
 * This way the user will have to first run the raid, then supply additional
 * information using the sysfs and can then begin to do something with it.
 **/
struct raidxor_conf {
	mddev_t *mddev;
 	spinlock_t device_lock;

	unsigned int status;

	unsigned long chunk_size;
	sector_t stripe_size;

	wait_queue_head_t wait_for_line;

	cache_t *cache;

	unsigned int configured;

	unsigned long units_per_resource;
	unsigned long n_resources;
	resource_t **resources;

	unsigned long n_stripes;
	stripe_t **stripes;

	unsigned long n_units;
	disk_info_t units[0];
};

#define mddev_to_conf(mddev) (mddev->private)

#define LOCKCONF(conf) \
	spin_lock(&conf->device_lock)

#define UNLOCKCONF(conf) \
	spin_unlock(&conf->device_lock)

#define WITHLOCKCONF(conf,block) \
	LOCKCONF(conf); \
	do block while(0); \
	UNLOCKCONF(conf);

#define RAIDXOR_CONF_STATUS_NORMAL 0
#define RAIDXOR_CONF_STATUS_INCOMPLETE 1
#define RAIDXOR_CONF_STATUS_STOPPING 2
/* #define RAIDXOR_CONF_STATUS_ERROR 2 */



/**
 * struct raidxor_bio - private information for bio transfers from and to stripes
 * @remaining: the number of remaining transfers
 * @mddev: the raid device
 * @stripe: the stripe this bio is transfering from or to
 * @sector: the virtual sector address inside that stripe
 * @unit: extra information from raidxor
 * @bios: the bios to the individual units
 *
 * If remaining reaches zero, the whole transfer is finished.
 */
struct raidxor_bio {
	atomic_t remaining;
	cache_t *cache;
	cache_line_t *line;

	stripe_t *stripe;
	sector_t sector;
	unsigned long length;

	unsigned long n_bios;
	struct bio *bios[0];
};

#define CHECK_LEVEL KERN_EMERG
#define CHECK(pointer,label,message) \
	if (!(pointer)) { \
		printk(CHECK_LEVEL "%s:%u:raidxor: check failed: %s\n", \
		       __FILE__, __LINE__, message); \
		goto label; \
	}

#endif
