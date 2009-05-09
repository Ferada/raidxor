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
	disk_info_t units[0];
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
	disk_info_t units[0];
};



/**
 * struct stripe - one stripe over multiple resources and units
 * @n_units: the number of contained units
 * @units: the actual units
 *
 * In the rectangular raid layout, this is a column of units.
 *
 * This is the correct level for en- and decoding.
 */
struct stripe {
	unsigned int n_units;
	disk_info_t units[0];
};



/**
 * struct raidxor_private_data_s - private data per mddev
 * @mddev: the mddev we are associated to
 * @device_lock: lock for exclusive access to this raid
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

	//mempool_t *rxbio_pool;

	/* not yet used */
	//struct bio_list pending_bio_list;

	unsigned int configured;

	unsigned long units_per_resource;
	unsigned long n_resources;
	raidxor_resource_t *resources;

	unsigned long n_stripes;
	stripe_t *stripes;

	unsigned long n_units;
	disk_info_t units[0];
};

#define mddev_to_conf(mddev) (mddev->private)



/**
 * struct raidxor_bio - private information for bio transfers from and to stripes
 * @remaining: the number of remaining transfers
 * @mddev: the raid device
 * @master_bio: the bio which was sent to the raid device
 * @bios: the bios to the individual units
 *
 * If remaining reaches zero, the whole transfer is finished.
 */
struct raidxor_bio {
	atomic_t remaining;
	mddev_t *mddev;

	/* original bio going to /dev/mdX */
	struct bio *master_bio;

	struct bio *bios[0];
};

#endif
