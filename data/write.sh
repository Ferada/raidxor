#!/bin/sh

FROM=${3:-/dev/zero}

dd if=$FROM of=/dev/md0 bs=512 count=$1 seek=$2
