#ifndef _RAIDXOR_H
#define _RAIDXOR_H

#include <linux/raid/md.h>

/* new raid level e.g. for mdadm */
#define LEVEL_XOR (-10)

/* private data per mddev */
struct raidxor_private_data_s {
	mddev_t *mddev;
};

typedef struct raidxor_private_data_s conf_t;

#define mddev_to_conf(mddev) ((conf_t *) mddev->private)

#endif
