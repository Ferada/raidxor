#!/bin/env python

import sys, os, os.path, fileinput, re
from optparse import OptionParser

"""
args:
  --start	start the raid
  --stop	stop the raid
  --restart	restart the raid

  -s, --script	generate shell script from specification
  -e, --exec	execute the specification after reading

format:

RAID_DESCR /dev/md0, chunk_size

RESOURCES r0,r1,...,rn
RESOURCE_DESCR r0,device,u0,u3,...,un

UNITS u0,u1,...,un
UNIT_DESCR u0,device

REDUNDANCY destunit = XOR(u1,u5,...,un)
"""

parser = OptionParser ()
parser.add_option ("--start", dest = "mode",
                   action = "store_const", const = "start",
                   help = "start the raid")
parser.add_option ("--stop", dest = "mode",
                   action = "store_const", const = "stop",
                   help = "stop the raid")
parser.add_option ("--restart", dest = "mode",
                   action = "store_const", const = "restart",
                   help = "restart the raid")
parser.add_option ("-n", "--noscript", dest = "script",
                   action = "store_false", default = True,
                   help = "writes no shell script to stdout")
parser.add_option ("-e", "--exec", dest = "exec",
                   action = "store_true", default = False,
                   help = "executes the specification")
parser.set_usage ("""Usage: conf.py [options]

Constructs a shell script from the specification on stdin or otherwise
just executes the actions needed to perform the mode change on the raid.

The format for the specification is as follows:

  RAID_DESCR device, chunk_size

  RESOURCES r0, r1, ..., rn
  RESOURCE_DESCR r0, device, u0, u1, ..., un

  UNITS u0, u1, ..., un
  UNIT_DESCR u0, device

  REDUNDANCY destunit = XOR(u0, u1, ..., un)""")

(opts, args) = parser.parse_args ()

global raid_device, chunk_size, resources, units
raid_device = None
chunk_size = 0
resources = []
units = []

class resource ():
    def __init__ (self, name, device = None, units = []):
        self.name = name
        self.device = device
        self.units = units
    def __repr__ (self):
        return "<resource %s on %s, %s>" % (self.name, self.device, [unit.name for unit in self.units])

class unit ():
    def __init__ (self, name, device = None, redundant = False, encoding = []):
        self.name = name
        self.device = device
        self.redundant = redundant
        self.encoding = encoding
    def __repr__ (self):
        if self.redundant:
            title = "redundant "
            red = ", %s" % [unit.name for unit in self.encoding]
        else:
            title = ""
            red = ""
        return "<%sunit %s on %s%s>" % (title, self.name, self.device, red)

def die (msg):
    sys.stderr.write (msg + "\n")
    sys.exit (-1)

def decorate_matcher (regex):
    def result (func):
        def wrap (line):
            match = regex.match (line)
            if not match:
                return False
            func (line, match)
            return True
        return wrap
    return result
match = decorate_matcher

def convert_resources ():
    global resources
    for res in resources:
        res.units = [find_unit (name) for name in res.units]

def parse_units (units):
    return [unit for unit in unit_re.split (units) if unit != ""]

def convert_units (names):
    global units
    return [find_unit (name) for name in names]

def find_unit (name):
    global units
    unit = filter (lambda x: x.name == name, units)
    if not unit:
        die ("unknown unit '%s'" % name)
    return unit[0]

unit_re = re.compile("\s*(\w+)\s*,\s*")

@match(re.compile ("^RAID_DESCR\s+([^\s]+)\s*,\s*(\d+)\s*$"))
def handle_raid_desc (line, match):
    global raid_device, chunk_size, resources, units
    (raid_device, chunk_size) = match.groups ()
    chunk_size = int (chunk_size)
    resources = []
    units = []

    if chunk_size < 4096:
        die ("invalid chunk_size '%s' < 4096" % chunk_size)

@match(re.compile ("^RESOURCES\s+(.*\w.*)$"))
def handle_res (line, match):
    global resources

    if resources:
        die ("refusing to overwrite resources")

    resources = [resource (res) for res in parse_units (match.groups ()[0])]
    
@match(re.compile ("^RESOURCE_DESCR\s+(\w+)\s*,\s*([^\s]+)\s*,\s*(.*\w.*)$"))
def handle_res_desc (line, match):
    global resources
    (name, device, units) = match.groups ()

    res = filter (lambda x: x.name == name, resources)
    if not res:
        die ("unknown resource '%s'" % name)
    res = res[0]

    res.device = device
    if res.units:
        sys.stderr.write ("overwriting description for %s\n" % res.name)
    res.units = parse_units (units)

@match(re.compile ("^UNITS\s+(.*\w.*)$"))
def handle_units (line, match):
    global units

    if units:
        die ("refusing to overwrite units")

    units = [unit (tmp, None) for tmp in parse_units (match.groups ()[0])]

@match(re.compile ("^UNIT_DESCR\s+(\w+)\s*,\s*([^\s]+)\s*$"))
def handle_unit_desc (line, match):
    global units
    (name, device) = match.groups ()

    unit = find_unit (name)
    if unit.device:
        sys.stderr.write ("overwriting description for %s\n" % unit.name)
    unit.device = device

@match(re.compile ("^REDUNDANCY\s+(\w+)\s*=\s*XOR\s*\(([^\)]*)\)"))
def handle_red (line, match):
    global units
    (name, red) = match.groups ()

    unit = find_unit (name)
    unit.redundant = True
    if unit.encoding:
        sys.stderr.write ("overwriting encoding for %s\n" % unit.name)
    unit.encoding = convert_units (parse_units (red))

matchers = [
    handle_raid_desc,
    handle_res,
    handle_res_desc,
    handle_units,
    handle_unit_desc,
    handle_red,
]

def try_matchers (line):
    for matcher in matchers:
        if matcher (line):
            return True
    return False

for line in fileinput.input (["-"]):
    if line[-1] == "\n":
        line = line[:-1]
    if len (line) == 0:
        continue

    if line[0] == "#" or line == "":
        continue

    if try_matchers (line):
        continue
    else:
        sys.stderr.write ("invalid line '%s'\n" % line)

def check_double (list, desc):
    num = {}
    def inc (key):
        if num.has_key (key):
            num[key] += 1
        else:
            num[key] = 1
    [inc (res.name) for res in list]
    for key, value in num.iteritems ():
        if value != 1:
            die ("double %s %s" % (desc, key))

def check_raid ():
    if not raid_device:
        die ("no RAID_DESCR line seen")
    if not resources:
        die ("no resources specified")
    if not units:
        die ("no units specified")

def generate_shell_script ():
    sys.stdout.write (
"""#!/bin/sh

MDADM=/home/rudolf/src/mdadm-2.6.8/mdadm

$MDADM -v -v --create 
""")

convert_resources ()
check_double (resources, "resource")
check_double (units, "unit")
check_raid ()

print raid_device
print chunk_size
print resources
print units

sys.exit (0)
