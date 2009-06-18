#!/bin/sh

COPY=copy.img
LOOP=/dev/loop10

if [[ ! -e $COPY ]]
then
	dd if=/dev/zero of=$COPY bs=1 count=1 seek=128M
	losetup $LOOP $COPY
	mkfs -t ext3 $LOOP
	losetup -d $LOOP
fi
