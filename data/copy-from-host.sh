#!/bin/sh

MOUNT=/mnt/copy

mount -t ext3 /dev/sdb10 $MOUNT
cp $MOUNT/$1 $2
umount $MOUNT
