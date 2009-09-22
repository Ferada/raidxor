#!/bin/env python

import sys, os, os.path, fileinput, re, popen2
from optparse import OptionParser

parser = OptionParser ()
parser.set_defaults (mode = "start")
parser.add_option ("--start", dest = "mode",
                   action = "store_const", const = "start",
                   help = "start the raid")
parser.add_option ("--stop", dest = "mode",
                   action = "store_const", const = "stop",
                   help = "stop the raid")
parser.add_option ("--restart", dest = "mode",
                   action = "store_const", const = "restart",
                   help = "restart the raid")
parser.add_option ("--decode", dest = "mode",
                   action = "store_const", const = "decode",
                   help = "serves kernel decoding requests")
parser.add_option ("--cauchyrs", dest = "cauchyrs",
                   default = "", metavar = "FILE",
                   help = "specifies cauchyrs executable file")
parser.add_option ("--cauchyrs-opts", dest = "cauchyopts",
		   default = "", metavar = "OPTS",
		   help = "specifies the options for cauchyrs")
parser.add_option ("--mdadm", dest = "mdadm",
		   default = "./mdadm", metavar = "FILE",
		   help = "specifies mdadm executable file")
parser.add_option ("-n", "--noscript", dest = "script",
                   action = "store_false", default = True,
                   help = "writes no shell script to stdout")
parser.add_option ("-e", "--exec", dest = "execute",
                   action = "store_true", default = False,
                   help = "executes the specification")
parser.set_usage ("""Usage: conf.py [options]

Constructs a shell script from the specification on stdin or otherwise
just executes the actions needed to perform the mode change on the raid.

The format for the specification is as follows:

  RAID_DESCR device, chunk_size

  RESOURCES r0, r1, ..., rn
  RESOURCE_DESCR r0, /dev/foo, u1, u3, ..., un

  UNITS u0, u1, ..., un
  UNIT_DESCR u0, /dev/bar

  REDUNDANCY destunit = XOR(u4, u2, ..., un)

and additionally for decoding equations

  DECODING destunit = XOR(u4, u2, ..., un)""")

(opts, args) = parser.parse_args ()

global raid_device, chunk_size, resources, units
raid_device = None
chunk_size = 0
resources = []
units = []

class resource ():
    def __init__ (self, name, device = None, units = [], faulty = False):
        self.name = name
        self.device = device
        self.units = units
        self.faulty = faulty
    def __repr__ (self):
        title = ""
        if self.faulty:
            if title != "":
                title += ", "
            title += "faulty "
        return "<%sresource %s on %s, %s>" % (title, self.name, self.device, [unit.name for unit in self.units])

class unit ():
    def __init__ (self, name, device = None, redundant = False, encoding = [], decoding = [], resource = None):
        self.name = name
        self.device = device
        self.redundant = redundant
        self.encoding = encoding
        self.decoding = None
        self.faulty = False
        self.resource = resource
    def __repr__ (self):
        title = ""
        red = ""
        if self.redundant:
            title = "redundant "
            red = ", %s" % [unit.name for unit in self.encoding]
        if self.faulty:
            if title != "":
                title += ", "
            title += "faulty "
        return "<%sunit %s on %s%s>" % (title, self.name, self.device, red)
    def mdname (self):
        return os.path.basename (self.device)

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
        res.units = [find_unit (unit.name) for unit in res.units]

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

unit_re = re.compile ("\s*(\w+)\s*,\s*")

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

@match(re.compile ("^REDUNDANCY\s+(\w+)\s*=\s*\(([^\)]*)\)"))
def handle_red (line, match):
    global units
    (name, red) = match.groups ()

    unit = find_unit (name)
    unit.redundant = True
    if unit.encoding:
        sys.stderr.write ("overwriting encoding for %s\n" % unit.name)
    unit.encoding = convert_units (parse_units (red))

@match(re.compile ("^DECODING\s+(\w+)\s*=\s*\(([^\)]*)\)"))
def handle_dec (line, match):
    global units
    (name, dec) = match.groups ()

    unit = find_unit (name)
    if unit.decoding:
        sys.stderr.write ("overwriting decoding for %s\n" % unit.name)
    unit.decoding = convert_units (parse_units (dec))

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

def fixup_resources_units ():
    global units, resources
    for unit in units:
        for resource in resources:
            try:
                match = resource.units.index (unit.name)
                resource.units[match] = unit
                unit.resource = resource
                break
            except ValueError:
                pass

fixup_resources_units ()

def check_double (list, desc, obj = None):
    num = {}
    def inc (key):
        if num.has_key (key):
            num[key] += 1
        else:
            num[key] = 1
    [inc (res.name) for res in list]
    for key, value in num.iteritems ():
        if value != 1:
            if obj:
                tmp = " for " + obj
            else:
                tmp = ""
            die ("double %s %s%s" % (desc, key, tmp))

def check_resources ():
    for res in resources:
        check_double (res.units, "unit", res.name)

def check_units ():
    for unit in units:
        if not unit.redundant:
            continue

        check_double (unit.encoding, "encoding", unit.name)

def check_raid ():
    if not raid_device:
        die ("no RAID_DESCR line seen")
    if not resources:
        die ("no resources specified")
    if not units:
        die ("no units specified")
    if chunk_size < 4096:
        die ("chunk_size is smaller than the minimum 4096 bytes")
    if chunk_size % 4096:
        die ("chunk_size is no multiple of 4096 bytes")

def check_rect_layout ():
    l = len (resources[0].units)
    for res in resources:
        if len (res.units) != l:
            die ("no rectangular layout, resource dimensions differ")
    if len (units) != len (resources) * l:
        die ("no rectangular layout, some units are amiss")

convert_resources ()
check_resources ()
check_double (resources, "resource")
check_double (units, "unit")
check_units ()
check_raid ()
check_rect_layout ()

def block_name (device):
    return os.path.basename (device)

def generate_encoding_shell_script (out):
    global units

    for i in range (0, len (units)):
        if not units[i].redundant:
            out.write ("""echo -en '\\0%s\\00""" % (oct (i)))
        else:
            out.write ("""echo -en '\\0%s\\01\\0%s""" % (oct (i), oct (len (units[i].encoding))))
            for unit in units[i].encoding:
                out.write ("""\\0%s""" % (oct (units.index (unit))))
        out.write ("' > /sys/block/%s/md/encoding\n" % (block_name (raid_device)))

def generate_decoding_shell_script (out):
    global units

    for i in range (0, len (units)):
        if not units[i].faulty:
            continue
        out.write ("""echo -en '\\0%s\\0%s""" % (oct (i), oct (len (units[i].decoding))))
        for unit in units[i].decoding:
            out.write ("""\\0%s""" % (oct (units.index (unit))))
        out.write ("' > /sys/block/%s/md/decoding\n" % (block_name (raid_device)))

def generate_stop_shell_script (out):
    out.write (
"""#!/bin/sh

MDADM=%s

$MDADM --manage %s -S
""" % (opts.mdadm, raid_device))

def generate_start_shell_script (out):
    units_formatted = ""
    for unit in units:
        units_formatted += " " + unit.device
    out.write (
"""#!/bin/sh

MDADM=%s

$MDADM -v -v --create %s -R -c %s --level=xor \\
	--raid-devices=%s%s
if [[ ! $? -eq 0 ]]; then exit; fi

""" % (opts.mdadm, raid_device, chunk_size / 1024, len (units), units_formatted))
    generate_encoding_shell_script (out)
    out.write (
"""
echo %s > /sys/block/%s/md/resources_per_stripe
echo %s > /sys/block/%s/md/units_per_resource
""" % (len (resources), block_name (raid_device), len (resources[0].units), block_name (raid_device)))

def parse_faulty ():
    global units

    for unit in units:
	path = "/sys/block/%s/md/dev-%s/state" % (block_name (raid_device), unit.mdname ())
	for line in fileinput.input (path):
            unit.faulty = (line.find("faulty") >= 0)
            if unit.faulty:
                unit.resource.faulty = True
            fileinput.close ()
            break

def parse_cauchyrs ():
    global resources

    failed = ""
    n = 0
    for i in range (0, len (resources)):
        print resources[i]
        if resources[i].faulty:
            failed += " -f%s=%s" % (n, i)
            n += 1
    cmdline = "%s %s %s" % (opts.cauchyrs, opts.cauchyopts, failed)
    print cmdline

    (subout, subin) = popen2.popen2 (cmdline)
    for line in subout:
        handle_dec (line)

files = []
if opts.script:
    files.append (sys.stdout)

if opts.execute:
    files.append (os.popen ("/bin/sh", "w"))

if opts.mode == "stop" or opts.mode == "restart":
    [generate_stop_shell_script (file) for file in files]
if opts.mode == "start" or opts.mode == "start":
    [generate_start_shell_script (file) for file in files]
if opts.mode == "decode":
    print "cauchyrs executable at %s" % opts.cauchyrs
    parse_faulty ()
    if any ([resource.faulty for resource in resources]):
        parse_cauchyrs ()
        [generate_decoding_shell_script (file) for file in files]

sys.exit (0)
