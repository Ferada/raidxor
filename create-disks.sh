#!/bin/sh

for i in 0 1 2 3 4 5
do
    if [[ ! -e disk-$i.img ]]; then
	dd if=/dev/zero of=disk-$i.img bs=1 count=1 seek=1G
    fi
done
