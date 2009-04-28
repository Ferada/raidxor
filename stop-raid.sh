#!/bin/sh

MDADM=/home/rudolf/src/mdadm-2.6.8/mdadm

$MDADM --manage /dev/md0 -S

for i in 0 1 2 3 4
do
    if [[ ! -e /dev/loop$i ]]; then
	losetup -d /dev/loop$i
    fi
done
