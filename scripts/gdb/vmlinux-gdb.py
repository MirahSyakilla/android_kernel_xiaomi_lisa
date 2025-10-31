#
# gdb helper commands and functions for Linux kernel debugging
#
#  loader module
#
# Copyright (c) Siemens AG, 2012, 2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import os
import sys

# Construct the path to the 'linux' helper scripts directory
# (assuming they are in 'scripts/gdb' relative to this file)
# and add it to Python's module search path.
# Py3: Using os.path.join is more robust than string concatenation.
# Using abspath makes it resilient to how the script is sourced.
scripts_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scripts", "gdb")
sys.path.insert(0, scripts_path)

# Check if the GDB version is new enough to support the Python API features
# used by these scripts (e.g., gdb.execute with to_string=True).
try:
    gdb.parse_and_eval("0")
    gdb.execute("", to_string=True)
# Py3: A bare 'except:' is bad practice. Catching Exception is safer.
except Exception:
    gdb.write("NOTE: A modern GDB version (7.2+) is required for the Linux "
              "kernel helper scripts to work.\n")
else:
    # If the GDB API check passes, import all the helper modules.
    # This registers all the GDB commands and functions they contain.
    import linux.utils
    import linux.symbols
    import linux.modules
    import linux.dmesg
    import linux.tasks
    import linux.config
    import linux.cpus
    import linux.lists
    import linux.rbtree
    import linux.proc
    import linux.constants
    import linux.timerlist
    import linux.clk
    import linux.genpd
    import linux.device