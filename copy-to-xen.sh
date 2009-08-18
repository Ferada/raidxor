#!/bin/sh

LOOP=/dev/loop10
MOUNT=/mnt/copy

mount -t ext3 $LOOP $MOUNT
cp $* $MOUNT
umount $MOUNT
