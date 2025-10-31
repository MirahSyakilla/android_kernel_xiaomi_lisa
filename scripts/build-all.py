#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2009-2015, 2017-19, The Linux Foundation. All rights reserved.

# Build the kernel for all targets using the Android build environment.

from collections import namedtuple
import glob
# Py3: argparse is the modern replacement for the deprecated optparse.
from argparse import ArgumentParser
import os
import re
import shutil
import subprocess
import sys
import threading
# Py3: The 'Queue' module was renamed to 'queue' in Python 3.
from queue import Queue

version = 'build-all.py, version 1.99 (Python 3 compatible)'

build_dir = '../all-kernels'
make_command = ["vmlinux", "modules", "dtbs"]
# Py3: Removed 'all_options' global; options are now passed as arguments.
compile64 = os.environ.get('CROSS_COMPILE64')
clang_bin = os.environ.get('CLANG_BIN')

def error(msg):
    # Py3: Use print() function with file=sys.stderr. f-string for formatting.
    print(f"error: {msg}", file=sys.stderr)

def fail(msg):
    """Fail with a user-printed message"""
    error(msg)
    sys.exit(1)

if not os.environ.get('CROSS_COMPILE'):
    fail("CROSS_COMPILE must be set in the environment")

def check_kernel():
    """Ensure that PWD is a kernel directory"""
    if not os.path.isfile('MAINTAINERS'):
        fail("This doesn't seem to be a kernel dir")

def check_build():
    """Ensure that the build directory is present."""
    if not os.path.isdir(build_dir):
        try:
            # Py3: os.makedirs has an 'exist_ok' flag, which simplifies this.
            os.makedirs(build_dir, exist_ok=True)
        except OSError as exc:
            # This is a fallback in case of a race condition, but unlikely.
            error(f"Failed to create build directory: {exc}")
            raise

failed_targets = []

BuildResult = namedtuple('BuildResult', ['status', 'messages'])

class BuildSequence(namedtuple('BuildSequence', ['log_name', 'short_name', 'steps'])):

    def set_width(self, width):
        self.width = width

    def __enter__(self):
        # Py3: 'w' mode handles text encoding automatically.
        self.log = open(self.log_name, 'w')
        return self

    def __exit__(self, type, value, traceback):
        self.log.close()

    def run(self):
        self.status = None
        messages = ["Building: " + self.short_name]
        def printer(line):
            # Py3: f-string for cleaner formatting.
            text = f"[{self.short_name:<{self.width}}] {line}"
            messages.append(text)
            self.log.write(text)
            self.log.write('\n')
        for step in self.steps:
            st = step.run(printer)
            if st:
                self.status = BuildResult(self.short_name, messages)
                break
        if not self.status:
            self.status = BuildResult(None, messages)

class BuildTracker:
    """Manages all of the steps necessary to perform a build.  The
    build consists of one or more sequences of steps.  The different
    sequences can be processed independently, while the steps within a
    sequence must be done in order."""

    def __init__(self, parallel_builds, verbose=False):
        self.sequence = []
        self.lock = threading.Lock()
        self.parallel_builds = parallel_builds
        self.verbose = verbose

    def add_sequence(self, log_name, short_name, steps):
        self.sequence.append(BuildSequence(log_name, short_name, steps))

    def longest_name(self):
        longest = 0
        for seq in self.sequence:
            longest = max(longest, len(seq.short_name))
        return longest

    def __repr__(self):
        return f"BuildTracker({self.sequence})"

    def run_child(self, seq):
        seq.set_width(self.longest)
        tok = self.build_tokens.get()
        with self.lock:
            # Py3: print is a function.
            print("Building:", seq.short_name)
        with seq:
            seq.run()
            self.results.put(seq.status)
        self.build_tokens.put(tok)

    def run(self):
        self.longest = self.longest_name()
        self.results = Queue()
        children = []
        errors = []
        self.build_tokens = Queue()
        nthreads = self.parallel_builds
        # Py3: print is a function.
        print(f"Building with {nthreads} threads")
        for i in range(nthreads):
            self.build_tokens.put(True)
        for seq in self.sequence:
            child = threading.Thread(target=self.run_child, args=[seq])
            children.append(child)
            child.start()
        for child in children:
            stats = self.results.get()
            if self.verbose:
                with self.lock:
                    for line in stats.messages:
                        print(line)
                    sys.stdout.flush()
            if stats.status:
                errors.append(stats.status)
        for child in children:
            child.join()
        if errors:
            fail("\n  ".join(["Failed targets:"] + errors))

class PrintStep:
    """A step that just prints a message"""
    def __init__(self, message):
        self.message = message

    def run(self, outp):
        outp(self.message)
        return None # Py3: Explicitly return None for clarity

class MkdirStep:
    """A step that makes a directory"""
    def __init__(self, direc):
        self.direc = direc

    def run(self, outp):
        outp(f"mkdir {self.direc}")
        os.mkdir(self.direc)
        return None

class RmtreeStep:
    def __init__(self, direc):
        self.direc = direc

    def run(self, outp):
        outp(f"rmtree {self.direc}")
        shutil.rmtree(self.direc, ignore_errors=True)
        return None

class CopyfileStep:
    def __init__(self, src, dest):
        self.src = src
        self.dest = dest

    def run(self, outp):
        outp(f"cp {self.src} {self.dest}")
        shutil.copyfile(self.src, self.dest)
        return None

class ExecStep:
    def __init__(self, cmd, **kwargs):
        self.cmd = cmd
        self.kwargs = kwargs

    def run(self, outp):
        outp(f"exec: {' '.join(self.cmd)}")
        with open(os.devnull, 'r') as devnull:
            proc = subprocess.Popen(self.cmd, stdin=devnull,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    # Py3: Popen expects text mode to be explicitly requested.
                    # Here we want bytes, so we leave it as default.
                    **self.kwargs)
            stdout = proc.stdout
            while True:
                # Py3: Subprocess PIPE reads bytes, not strings.
                line_bytes = stdout.readline()
                if not line_bytes:
                    break
                # Py3: Decode bytes to string, ignoring errors, and strip newline.
                line = line_bytes.decode('utf-8', errors='ignore').rstrip('\n')
                outp(line)
            result = proc.wait()
            if result != 0:
                return ('error', result)
            else:
                return None

class Builder():

    def __init__(self, name, defconfig):
        self.name = name
        self.defconfig = defconfig

        self.confname = re.sub('arch/arm[64]*/configs/', '', self.defconfig)

        # Determine if this is a 64-bit target based on the location
        # of the defconfig.
        self.make_env = os.environ.copy()
        if "/arm64/" in defconfig:
            if compile64:
                self.make_env['CROSS_COMPILE'] = compile64
            else:
                fail("Attempting to build 64-bit, without setting CROSS_COMPILE64")
            self.make_env['ARCH'] = 'arm64'
        else:
            self.make_env['ARCH'] = 'arm'
        self.make_env['KCONFIG_NOTIMESTAMP'] = 'true'
        self.log_name = f"{build_dir}/log-{self.name}.log"

    def build(self):
        steps = []
        dest_dir = os.path.join(build_dir, self.name)
        log_name = f"{build_dir}/log-{self.name}.log"
        steps.append(PrintStep(f'Building {self.name} in {dest_dir} log {log_name}'))
        if not os.path.isdir(dest_dir):
            steps.append(MkdirStep(dest_dir))

        staging_dir = 'install_staging'
        modi_dir = f'{staging_dir}'
        hdri_dir = f'{staging_dir}/usr'
        steps.append(RmtreeStep(os.path.join(dest_dir, staging_dir)))

        steps.append(ExecStep(['make', f'O={dest_dir}',
            self.confname], env=self.make_env))

        # Build targets can be dependent upon the completion of
        # previous build targets, so build them one at a time.
        cmd_line = ['make',
            f'INSTALL_HDR_PATH={hdri_dir}',
            f'INSTALL_MOD_PATH={modi_dir}',
            f'O={dest_dir}',
            f'REAL_CC={clang_bin}']
        build_targets = []
        for c in make_command:
            if re.match(r'^-{1,2}\w', c):
                cmd_line.append(c)
            else:
                build_targets.append(c)
        for t in build_targets:
            steps.append(ExecStep(cmd_line + [t], env=self.make_env))

        return steps

def scan_configs():
    """
       Get the full list of defconfigs appropriate for this tree,
       except for gki_defconfig.
    """
    names = []
    for defconfig in glob.glob('arch/arm*/configs/vendor/*_defconfig'):
        target = os.path.basename(defconfig)[:-10]
        if 'gki' == target:
            continue
        name = target + "-llvm"
        if 'arch/arm64' in defconfig:
            name = name + "-64"
        names.append(Builder(name, defconfig))

    return names

def build_many(targets, options):
    print(f"Building {len(targets)} target(s)")

    # To try and make up for the link phase being serial, try to do
    # two full builds in parallel.  Don't do too many because lots of
    # parallel builds tends to use up available memory rather quickly.
    parallel = 2
    if options.jobs and options.jobs > 1:
        j = max(options.jobs // parallel, 2)
        make_command.append(f"-j{j}")

    tracker = BuildTracker(parallel, verbose=options.verbose)
    for target in targets:
        steps = target.build()
        tracker.add_sequence(target.log_name, target.name, steps)
    tracker.run()

def main():
    global make_command

    check_kernel()
    check_build()

    configs = scan_configs()

    # Py3: Switched from optparse to argparse
    description = """
Build the kernel for all targets or a specified list of targets.
Examples:
  %prog all                 -- Build all targets
  %prog target1 target2 ... -- Build specific targets
"""
    parser = ArgumentParser(description=description.replace('%prog', sys.argv[0]))
    parser.add_argument('--version', action='version', version=version)
    parser.add_argument('--list', action='store_true',
            help='List available targets')
    parser.add_argument('-v', '--verbose', action='store_true',
            help='Output to stdout in addition to log file')
    parser.add_argument('-j', '--jobs', type=int,
            help="Number of simultaneous jobs")
    parser.add_argument('-l', '--load-average', type=int,
            help="Don't start multiple jobs unless load is below LOAD_AVERAGE")
    parser.add_argument('-k', '--keep-going', action='store_true',
            default=False,
            help="Keep building other targets if a target fails")
    parser.add_argument('-m', '--make-target', action='append',
            help=f"Build the indicated make target (default: {' '.join(make_command)})")
    parser.add_argument('targets', nargs='*',
            help="A list of targets to build, or 'all'")

    options = parser.parse_args()

    if options.list:
        print("Available targets:")
        for target in configs:
            print(f"   {target.name}")
        sys.exit(0)

    if options.make_target:
        make_command = options.make_target

    if not options.targets:
        parser.error("Must specify a target to build (e.g., 'all')")

    if options.targets == ['all']:
        build_many(configs, options)
    else:
        all_configs = {t.name: t for t in configs}
        targets_to_build = []
        for t in options.targets:
            if t not in all_configs:
                # Py3: In Python 3, .keys() returns a view, so we convert to a list for nice printing.
                parser.error(f"Target '{t}' not one of {sorted(list(all_configs.keys()))}")
            targets_to_build.append(all_configs[t])
        build_many(targets_to_build, options)

if __name__ == "__main__":
    main()