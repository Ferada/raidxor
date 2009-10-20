#!/usr/bin/python

import sys

char = int(sys.argv[1])
num = int(sys.argv[2])

for i in range(1, num + 1):
  sys.stdout.write(chr(char))
