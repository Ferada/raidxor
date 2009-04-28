#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)

typedef struct disk_info disk_info_t;

struct disk_info {
	mdk_rdev_t *rdev;
};

/* private data per mddev */
struct raidxor_private_data_s {
	mddev_t *mddev;
	spinlock_t device_lock;

	mempool_t *rxbio_pool;

	/* not yet used */
	//struct bio_list pending_bio_list;

	/* the number of data disks from the beginning of disks[] */
	unsigned int n_data_disks;
	/* since we don't change this, create an array */
	disk_info_t disks[0];
};

typedef struct raidxor_private_data_s raidxor_conf_t;

#define mddev_to_conf(mddev) (mddev->private)

struct raidxor_bio_s {
	atomic_t remaining;
	mddev_t *mddev;

	/* original bio going to /dev/mdX */
	struct bio *master_bio;

	struct bio *bios[0];
};

typedef struct raidxor_bio_s raidxor_bio;

#endif
