#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)

struct disk_info {
	mdk_rdev_t *rdev;
};

/* private data per mddev */
struct raidxor_private_data_s {
	mddev_t *mddev;

	/* the number of data disks from the beginning of disks[] */
	unsigned int n_data_disks;
	/* since we don't change this, create an array */
	struct disk_info disks[1];
};

typedef struct raidxor_private_data_s raidxor_conf_t;

#define mddev_to_conf(mddev) (mddev->private)

#endif
