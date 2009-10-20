#!/bin/sh

N=65536

./gen.py 1 $N > /dev/sda1
./gen.py 3 $N > /dev/sda2
./gen.py 5 $N > /dev/sda3
./gen.py 7 $N > /dev/sdb1
./gen.py 9 $N > /dev/sdb2
./gen.py 11 $N > /dev/sdb3
./gen.py 13 $N > /dev/sdc1
./gen.py 17 $N > /dev/sdc2
./gen.py 19 $N > /dev/sdc3
./gen.py 23 $N > /dev/sdd1
./gen.py 27 $N > /dev/sdd2
./gen.py 31 $N > /dev/sdd3
./gen.py 37 $N > /dev/sde1
./gen.py 41 $N > /dev/sde2
./gen.py 43 $N > /dev/sde3
