#!/bin/sh

for i in 0 1 2 3 4
do
    dd if=/dev/zero of=disk-$i.img bs=1 count=1 seek=1G
done
