#!/bin/sh

if [ ! -e 128s ]
then
	./gen.py 128 65536 > 128s
fi

if [ ! -e 64s ]
then
	./gen.py 64 65536 > 64s
fi

if [ ! -e 192s ]
then
	./gen.py 192 65536 > 192s
fi

for i in 1 2 3 4 5 6
do
	dd if=/dev/zero of=/dev/sdb$i bs=512 count=128
done

dd if=32s of=/dev/sdb1
