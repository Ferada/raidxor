#!/bin/sh

TO=${3:-/dev/null}

dd if=/dev/md0 of=$TO bs=512 count=$1 skip=$2
