#!/bin/sh

MDADM=/home/rudolf/src/mdadm-2.6.8/mdadm

#$MDADM --manage /dev/md0 -S

for i in 0 1 2 3 4 5
do
    losetup -d /dev/loop$i
done

losetup -d /dev/loop10
