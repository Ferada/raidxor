#!/bin/sh

./stop-raid.sh
rmmod raidxor
insmod src/raidxor.ko
./start-raid.sh
