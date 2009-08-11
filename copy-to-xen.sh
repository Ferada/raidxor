#!/bin/sh

LOOP=/dev/loop10
MOUNT=/mnt/copy

mount $LOOP $MOUNT
cp $* $MOUNT
umount $MOUNT
