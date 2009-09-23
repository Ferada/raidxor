/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)

typedef struct disk_info disk_info_t;
typedef struct encoding encoding_t;
typedef struct decoding decoding_t;
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
 * @virtual_sector: index into the virtual device
 * @waiting: waiting requests
 * @buffers: actual data
 */
struct cache_line {
	unsigned long status;
	sector_t sector;

	raidxor_bio_t *rxbio;
	struct bio *waiting;

	struct page *buffers[0];
};

/**
 * struct cache - groups access to the individual cache lines
 * @active_lines: number of currently active read/write activities
 * @n_lines: number of lines
 * @n_buffers: number of actual buffers in each line
 * @n_red_buffers: number of redundant buffers in each line
 * @n_chunk_mult: number of buffers per chunk
 * @n_waiting: number of processes waiting for a free line
 * @wait_for_line: waitqueue so we're able to wait for the event above
 *
 * device_lock needs to be hold when accessing the cache.
 */
struct cache {
	raidxor_conf_t *conf;
	unsigned int active_lines;
	unsigned int n_lines, n_buffers, n_red_buffers, n_chunk_mult;

	unsigned int n_waiting;
	wait_queue_head_t wait_for_line;

	cache_line_t *lines[0];
};

#define CACHE_LINE_CLEAN     0
#define CACHE_LINE_READY     1
#define CACHE_LINE_LOAD_ME   2
#define CACHE_LINE_LOADING   3
#define CACHE_LINE_UPTODATE  4
#define CACHE_LINE_DIRTY     5
#define CACHE_LINE_WRITEBACK 6
#define CACHE_LINE_FAULTY    7
#define CACHE_LINE_RECOVERY  8

static cache_t * raidxor_alloc_cache(unsigned int n_lines,
				     unsigned int n_buffers,
				     unsigned int n_red_buffers,
				     unsigned int n_chunk_mult);

/*
   Life cycle of a cache line:

                    /----------------------------------------- 10
                   v            	                      \
   +=======+ 1 	+-------+ 2  +---------+ 3 +---------+ 9 +----------+
   ¦ CLEAN ¦<==>| READY |--->| LOAD ME |-->| LOADING |-->| RECOVERY |
   +=======+  	+-------+    +---------+   +---------+   +----------+
		  ^		 	      4	 /             / 11
		  | 	 -----------------------/--------------
                  |     /
		 5|    /
                  |   v
                +----------+
                | UPTODATE |<-\ 8
                +----------+   ----\
         	 6 |	   	    \+----------+
		   v	 	  /->| WRITEBACK|
		+----------+  /---   +----------+
		|  DIRTY   |--  7
		+----------+

   1: allocating pages or dropping them
   2: we wan't to load some data from disk
   3: loading data from disk
   4: finishing read from disk
   5: dropping a cache line
   6: write some data to the cache
   7: start writeback process
   8: finishing writeback, data in cache and on device is either
      synchronised, or not synchronised but marked faulty
   9: we couldn't read from one device, start recovery
  10: recovery couldn't be finished (or started), so drop all requests and
      drop back to a more usable state
  11: recovery finished, so we are done

   during LOADING, RECOVERY and WRITEBACK, nothing is done in the handlers.
   the transition from clean to ready is simply memory (de-)allocation, so
   nothing fancy there.

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

   since we can't do anything about an error during writeback (apart from
   marking the device faulty), we just go to uptodate state (so it can be
   purged if necessary)

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
 * @decoding: contains the decoding equation if available
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
	decoding_t *decoding;

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
 * struct decoding - decoding information for a unit
 * @n_units: the number of contained units
 * @units: the actual units
 */
struct decoding {
	unsigned int n_units;
	disk_info_t *units[0];
};

/* static void optimized_parity(char *dest, char *src, unsigned long len); */
static int raidxor_xor_combine_encode(struct bio *bioto,
				      raidxor_bio_t *rxbio,
				      encoding_t *encoding);
static int raidxor_xor_combine_decode(struct bio *bioto,
				      raidxor_bio_t *rxbio,
				      decoding_t *decoding);


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
 * @size: size in blocks, that is, 512 bytes
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
	unsigned long flags;
	sector_t size;
	unsigned int n_data_units;
	unsigned int n_units;
	disk_info_t *units[0];
};

#define STRIPE_FAULTY 1
#define STRIPE_ERROR 2

/*
  STRIPE_FAULTY: we have a correctable error
  STRIPE_ERROR: no chance of recovery
 */

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

	unsigned long flags;

	unsigned long chunk_size;
	sector_t stripe_size;

	cache_t *cache;

	unsigned int configured;

	unsigned int resources_per_stripe;
	unsigned int units_per_resource;
	unsigned int n_resources;
	resource_t **resources;

	unsigned int n_stripes;
	stripe_t **stripes;

	unsigned int n_units;
	disk_info_t units[0];
};

#define mddev_to_conf(mddev) (mddev->private)

#if 0
#define LOCKCONF(conf) \
	spin_lock(&conf->device_lock)

#define UNLOCKCONF(conf) \
	spin_unlock(&conf->device_lock)
#else
#define LOCKCONF(conf, flags) \
	spin_lock_irqsave(&conf->device_lock, flags)

#define UNLOCKCONF(conf, flags) \
	spin_unlock_irqrestore(&conf->device_lock, flags)
#endif

#ifdef RAIDXOR_DEBUG
#define WITHLOCKCONF(conf,flags,block) \
	unsigned int __check_bug = spin_is_locked(&conf->device_lock); \
	if (__check_bug) CHECK_BUG("recursive lock"); \
	LOCKCONF(conf, flags); \
	do block while(0); \
	UNLOCKCONF(conf, flags);
#else
#define WITHLOCKCONF(conf,flags,block) \
	LOCKCONF(conf, flags); \
	do block while(0); \
	UNLOCKCONF(conf, flags);
#endif

/* #define RAIDXOR_CONF_STATUS_NORMAL 0 */
#define CONF_INCOMPLETE 1
#define CONF_FAULTY 2
#define CONF_STOPPING 4

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
	unsigned int remaining;
	cache_t *cache;
	unsigned int line;
	unsigned int faulty;

	stripe_t *stripe;

	unsigned int n_bios;
	struct bio *bios[0];
};

#define CHECK_LEVEL KERN_EMERG

#ifdef RAIDXOR_DEBUG
#define CHECK_HELPER(test,block,message) \
	if (!(test)) { \
		printk(CHECK_LEVEL "%s:%u:raidxor: check failed: %s\n", \
		       __FILE__, __LINE__, message); \
		block; \
	}
#else
#define CHECK_HELPER(test,block,message)
#endif

#define CHECK_RETURN_VALUE 1
#define CHECK_JUMP_LABEL out

#define CHECK_ARG_MESSAGE " missing"
#define CHECK_ALLOC_MESSAGE "couldn't allocate "

#define CHECK(test,message) \
	CHECK_HELPER(test, goto CHECK_JUMP_LABEL, message)
#define CHECK_RET(test,message) \
	CHECK_HELPER(test, return, message)
#define CHECK_RET_VAL(test,message) \
	CHECK_HELPER(test, return (CHECK_RETURN_VALUE), message)
#define CHECK_RET_NULL(test,message) \
	CHECK_HELPER(test, return NULL, message)

#define CHECK_PLAIN(test) \
	CHECK(test, #test)
#define CHECK_PLAIN_RET(test) \
	CHECK_RET(test, #test)
#define CHECK_PLAIN_RET_VAL(test) \
	CHECK_RET_VAL(test, #test)
#define CHECK_PLAIN_RET_NULL(test) \
	CHECK_RET_NULL(test, #test)

#define CHECK_ARG(arg) \
	CHECK(arg, #arg CHECK_ARG_MESSAGE)
#define CHECK_ARG_RET(arg) \
	CHECK_RET(arg, #arg CHECK_ARG_MESSAGE);
#define CHECK_ARG_RET_VAL(arg) \
	CHECK_RET_VAL(arg, #arg CHECK_ARG_MESSAGE);
#define CHECK_ARG_RET_NULL(arg) \
	CHECK_RET_NULL(arg, #arg CHECK_ARG_MESSAGE);

#define CHECK_ARGS2(arg1,arg2) \
	CHECK_ARG(arg1); CHECK_ARG(arg2)
#define CHECK_ARGS3(arg1,arg2,arg3) \
	CHECK_ARG(arg1); CHECK_ARGS2(arg2, arg3)

#define CHECK_ALLOC(var) \
	CHECK(var, CHECK_ALLOC_MESSAGE #var);
#define CHECK_ALLOC_RET(var) \
	CHECK_RET(var, CHECK_ALLOC_MESSAGE #var)
#define CHECK_ALLOC_RET_VAL(var) \
	CHECK_RET_VAL(var, CHECK_ALLOC_MESSAGE #var)
#define CHECK_ALLOC_RET_NULL(var) \
	CHECK_RET_NULL(var, CHECK_ALLOC_MESSAGE #var)

#ifdef RAIDXOR_DEBUG
#define CHECK_BUG(message) \
	printk(CHECK_LEVEL "raidxor: BUG at %s:%i: %s\n", \
	       __FILE__, __LINE__, message)

#define CHECK_LINE \
	printk(CHECK_LEVEL "raidxor: %s:%i\n", __FILE__, __LINE__)
#define CHECK_FUN(fun) \
	printk(CHECK_LEVEL "raidxor: %s:%i: %s\n", __FILE__, __LINE__, #fun)
#else
#define CHECK_BUG(message)
#define CHECK_LINE
#define CHECK_FUN(fun)
#define CHECK_STRIPE(conf)
#endif

#endif

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
