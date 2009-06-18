#!/bin/sh

LOOP=/dev/loop10
MOUNT=/mnt/copy

mount $MOUNT
cp $* $MOUNT
umount $MOUNT
