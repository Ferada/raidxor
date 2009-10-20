#!/bin/sh

MODULES=/lib/modules/2.6.27.7-9-xen/kernel/drivers/md/

LINES=${1:-10}

./mdadm --manage /dev/md0 -S
rmmod raidxor
./copy-from-host.sh raidxor.ko $MODULES
insmod $MODULES/raidxor.ko number_of_cache_lines=$LINES
