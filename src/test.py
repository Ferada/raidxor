#!/bin/env python

import sys, os, os.path
from optparse import OptionParser, OptionValueError
from threading import Thread
from random import randint

opt_str_to_name = {
    "--static" : "static",
    "--file" : "file",
    "--random-offset" : "random",
    "--random-length" : "random",
    "--fixed-offset" : "fixed",
    "--fixed-length" : "fixed",
}

def check_int (option, opt_str, value, parser):
    if opt_str_to_name.has_key (opt_str):
        setattr (parser.values, option.dest, [opt_str_to_name[opt_str], int (value)])
    else:
        raise OptionValueError ("option %s unknown" % opt_str)

parser = OptionParser ()
parser.set_defaults (data = ["random", 4096], offset = ["random", -1],
                     length = ["fixed", 1], bs = 512,
                     times = 1, parallel = 3, dev = "/dev/md0")
#parser.add_option ("--fixed-data", dest = "data", metavar = "BYTE",
#                   action = "callback", callback = check_int,
#                   nargs = 1, type = "string",
#                   help = "uses a given byte number as fixed data")
parser.add_option ("--file-data", dest = "data", metavar = "FILE",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "uses data in a file as source")
parser.add_option ("--random-data", dest = "data", metavar = "MAX",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "generates a random byte stream")

parser.add_option ("--fixed-offset", dest = "offset", metavar = "POS",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "reads from a fixed location")
parser.add_option ("--random-offset", dest = "offset", metavar = "MAX",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "reads from random locations")

parser.add_option ("--random-length", dest = "length",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "reads random chunks")
parser.add_option ("--fixed-length", dest = "length", metavar = "LEN",
                   action = "callback", callback = check_int,
                   nargs = 1, type = "string",
                   help = "reads fixed chunks")

parser.add_option ("-b", dest = "bs",
                   type = "int", metavar = "BLOCKSIZE",
                   help = "multiplicator for i/o operations")
parser.add_option ("-p", dest = "parallel",
                   type = "int", metavar = "PARALLEL",
                   help = "number of parallel requests")
parser.add_option ("-t", dest = "times",
                   type = "int", metavar = "TIMES",
                   help = "number of iterations")
parser.add_option ("-d", dest = "dev",
                   help = "actual device")

parser.set_usage ("""Usage: test.py [options]

Tests a device for consistency using multiple requests to specified or
random positions and sizes.""")

(opts, args) = parser.parse_args ()

print "data", opts.data
print "offset", opts.offset
print "length", opts.length
print "bs", opts.bs
print "parallel", opts.parallel
print "times", opts.times
print "args", args

def make_source ():
    def make_file_source ():
        return open (opts.data[1], "rb")

    def make_random_source ():
        dev = "/dev/urandom"
        return open (dev, "rb")

    if opts.data[0] == "random":
        return make_random_source ()
    elif opts.data[0] == "file":
        return make_file_source ()
    elif opts.data[0] == "fixed":
        return None
    else:
        raise Exception ("unknown data option %s" % opts.data)

def make_length ():
    if opts.length[0] == "fixed":
        return opts.length[1] * opts.bs
    elif opts.length[0] == "random":
        return randint (1, opts.length[1]) * opts.bs
    else:
        raise Exception ("unknown length option %s" % opts.length)

def make_offset ():
    if opts.offset[0] == "random":
        return randint (1, opts.offset[1]) * opts.bs
    elif opts.offset[0] == "fixed":
        return opts.offset[1] * opts.bs
    else:
        raise Exception ("unknown offset option %s" % opts.offset)

class VerifyReadWrite (Thread):
    def __init__ (self, dev, data, length, offset):
        self.device = dev
        self.data = data
        self.length = length
        self.offset = offset
        self.success = False
        self.written = None
        self.read = None
        Thread.__init__ (self)
    def run (self):
        try:
            dev = open (self.device, "rb+")
            try:
                print "seek", self.offset
                dev.seek (self.offset)
                self.written = self.data.read (self.length)
                dev.write (self.written)
                dev.seek (self.offset)
                self.read = dev.read (self.length)

                if self.written == self.read:
                    self.success = True
            finally:
                dev.close ()
        finally:
            self.data.close ()
    def successful (self):
        return self.success

def run_io ():
    def make_thread ():
        return VerifyReadWrite (opts.dev, make_source (),
                                make_length (), make_offset ())
    threads = [make_thread () for i in range (1, opts.parallel + 1)]
    failed = []
    for t in threads:
        t.start ()
    for t in threads:
        t.join ()
        if not t.successful ():
            failed.append (t)
    return failed

def dump (result, written, read):
	written = open (written, "wb")
	try:
		written.write (result.written)
	finally:
		written.close ()

	read = open (read, "wb")
	try:
		read.write (result.read)
	finally:
		read.close ()

def report (result, i, j):
	print "Report on round %d, test %d" % (i, j)
	print "offset: %db" % result.offset
	print "length: %db" % result.length
	print "written: %db" % len (result.written)
	print "read: %db" % len (result.read)
	print

for i in range (1, opts.times + 1):
	result = run_io ()
	if result == []:
		print "no failures"
	else:
		for j in range (0, len (result)):
			report (result[j], i, j)
			dump (result[j], "test-%d-%d-written" % (i, j), "test-%d-%d-read" % (i, j))
		break
