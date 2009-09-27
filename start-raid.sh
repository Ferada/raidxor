#!/bin/sh

MDADM=/home/rudolf/src/mdadm-2.6.8/mdadm

for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14
do
    losetup /dev/loop$i disk-$i.img
done

losetup /dev/loop20 copy.img

#$MDADM -v -v --create /dev/md0 -c 4 --level=xor \
#    --raid-devices=6 /dev/loop0 /dev/loop1 /dev/loop2 /dev/loop3 /dev/loop4 /dev/loop5
