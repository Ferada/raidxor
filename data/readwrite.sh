#!/bin/sh

FILE=${3:-testfile}
FILEIN=$FILE.in
FILEOUT=$FILE.out

BS=${BS:-512}

rm $FILEIN $FILEOUT
touch $FILEIN $FILEOUT

dd if=$FILE of=$FILEIN bs=$BS count=$1

dd if=$FILEIN of=/dev/md0 bs=$BS count=$1 seek=$2
dd if=/dev/md0 of=$FILEOUT bs=$BS count=$1 skip=$2

#diff -b $FILEIN $FILEOUT
md5sum $FILEIN
md5sum $FILEOUT
