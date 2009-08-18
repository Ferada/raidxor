#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)



typedef struct disk_info disk_info_t;
typedef struct encoding encoding_t;
typedef struct raidxor_bio raidxor_bio_t;
typedef struct raidxor_private_data_s raidxor_conf_t;
typedef struct stripe stripe_t;
typedef struct raidxor_resource raidxor_resource_t;
typedef struct cache cache_t;
typedef struct cache_line cache_line_t;



/**
 * struct cache_line - buffers multiple blocks over a stripe
 * @flags: current status of the line
 * @sector: index into the virtual device
 * @buffers: actual data
 */
struct cache_line {
	unsigned int flags;
	sector_t sector;

	struct page *buffers[0];
};



/**
 * struct cache - groups access to the individual cache lines
 *
 * device_lock needs to be hold when accessing the cache.
 */
struct cache {
	raidxor_conf_t *conf;
	unsigned int n_lines, n_buffers;

	cache_line_t lines[0];
};

#define CACHE_LINE_CLEAN 0
#define CACHE_LINE_READY 1
#define CACHE_LINE_LOADING 2
#define CACHE_LINE_UPTODATE 3
#define CACHE_LINE_DIRTY 4
#define CACHE_LINE_WRITEBACK 5

static cache_t * allocate_cache(unsigned int n_lines, unsigned int n_buffers);

/*
   Life cycle of a cache line:

   CLEAN: no pages are allocated

   +=======+ 1 	+-------+ 2  +---------+
   ¦ CLEAN ¦<==>| READY |<==>| LOADING |
   +=======+  	+-------+    +---------+
		  ^ 	   	---
		  | 	-------/
		 4|    /   3
                  |   v
                +----------+
                | UPTODATE |<-\ 7
                +----------+   ----\
         	 5 |	   	    \+----------+
		   v	 	  /=>| WRITEBACK|
		+----------+  /===   +----------+
		|  DIRTY   |<=  6
		+----------+
   1: allocating pages or dropping them
   2: loading data from disk
   3: finishing read from disk
   4: dropping a cache line
   5: write some data to the cache
   6: start writeback process
   7: finishing writeback, data in cache and on device is now synchronised

   if the writeback/loading fails, operation 6 is reversed, that is,
   writeback/loading is not successful, so we get back to dirty/ready

   requests are limited to multiple of PAGE_SIZE bytes, so all we have to do,
   is to take these requests, scatter their data into the cache, and write
   that back to disk (or load from there)
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

	raidxor_resource_t *resource;
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
 * @size: size in bytes but multiple of block size (512)
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



/**
 * struct raidxor_private_data_s - private data per mddev
 * @mddev: the mddev we are associated to
 * @device_lock: lock for exclusive access to this raid
 * @status: one of RAIDXOR_CONF_STATUS_{NORMAL,INCOMPLETE,ERROR}
 * @chunk_size: copied from mddev_t
 * @handle_list: requests needing handling
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
struct raidxor_private_data_s {
	mddev_t *mddev;
 	spinlock_t device_lock;

	unsigned int status;

	unsigned long chunk_size;

	struct list_head handle_list;

	cache_t *cache;

	unsigned int configured;

	unsigned long units_per_resource;
	unsigned long n_resources;
	raidxor_resource_t **resources;

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
/* #define RAIDXOR_CONF_STATUS_ERROR 2 */



/**
 * struct raidxor_bio - private information for bio transfers from and to stripes
 * @status: one of RAIDXOR_BIO_STATUS_{NORMAL,ERROR}
 * @remaining: the number of remaining transfers
 * @mddev: the raid device
 * @stripe: the stripe this bio is transfering from or to
 * @sector: the virtual sector address inside that stripe
 * @unit: extra information from raidxor
 * @master_bio: the bio which was sent to the raid device
 * @bios: the bios to the individual units
 *
 * If remaining reaches zero, the whole transfer is finished.
 */
struct raidxor_bio {
	struct list_head lru;

	atomic_t status;

	atomic_t remaining;
	mddev_t *mddev;

	stripe_t *stripe;
	sector_t sector;
	unsigned long length;

	/* original bio going to /dev/mdX */
	struct bio *master_bio;

	unsigned long n_bios;
	struct bio *bios[0];
};

#define RAIDXOR_BIO_STATUS_NORMAL 0

#endif
