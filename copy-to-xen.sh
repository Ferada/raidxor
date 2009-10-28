#!/bin/sh

LOOP=/dev/loop40
MOUNT=/mnt/copy

mount -t ext3 $LOOP $MOUNT
cp $* $MOUNT
umount $MOUNT
