#
# gdb helper commands and functions for Linux kernel debugging
#
#  kernel log buffer dump
#
# Copyright (c) Siemens AG, 2011, 2012
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
# Py3: sys is no longer needed for version checking.
# import sys

# Assuming 'linux.utils' is a GDB helper library compatible with the Python env.
from linux import utils

printk_log_type = utils.CachedType("struct printk_log")


class LxDmesg(gdb.Command):
    """Print Linux kernel log buffer."""

    def __init__(self):
        # Py3: Use the modern, argument-less super() call.
        super().__init__("lx-dmesg", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        # This logic for reading GDB values remains the same.
        log_buf_addr = int(str(gdb.parse_and_eval(
            "(void *)'printk.c'::log_buf")).split()[0], 16)
        log_first_idx = int(gdb.parse_and_eval("'printk.c'::log_first_idx"))
        log_next_idx = int(gdb.parse_and_eval("'printk.c'::log_next_idx"))
        log_buf_len = int(gdb.parse_and_eval("'printk.c'::log_buf_len"))

        inf = gdb.inferiors()[0]
        start = log_buf_addr + log_first_idx
        if log_first_idx < log_next_idx:
            log_buf_2nd_half = -1
            length = log_next_idx - log_first_idx
            # .tobytes() is correct for both Py2 and Py3
            log_buf = utils.read_memoryview(inf, start, length).tobytes()
        else:
            log_buf_2nd_half = log_buf_len - log_first_idx
            a = utils.read_memoryview(inf, start, log_buf_2nd_half)
            b = utils.read_memoryview(inf, log_buf_addr, log_next_idx)
            log_buf = a.tobytes() + b.tobytes()

        length_offset = printk_log_type.get_type()['len'].bitpos // 8
        text_len_offset = printk_log_type.get_type()['text_len'].bitpos // 8
        time_stamp_offset = printk_log_type.get_type()['ts_nsec'].bitpos // 8
        text_offset = printk_log_type.get_type().sizeof

        pos = 0
        # Py3: Using len() is more idiomatic than .__len__().
        while pos < len(log_buf):
            length = utils.read_u16(log_buf, pos + length_offset)
            if length == 0:
                if log_buf_2nd_half == -1:
                    gdb.write("Corrupted log buffer!\n")
                    break
                pos = log_buf_2nd_half
                continue

            text_len = utils.read_u16(log_buf, pos + text_len_offset)
            text_start = pos + text_offset
            # Py3: The result of slicing bytes is bytes, so .decode() is correct.
            text = log_buf[text_start:text_start + text_len].decode(
                encoding='utf8', errors='replace')
            time_stamp = utils.read_u64(log_buf, pos + time_stamp_offset)
            time_sec = time_stamp / 1000000000.0

            for line in text.splitlines():
                # Py3: The 'u' prefix is no longer needed (and is a syntax error
                # in modern Python). Using an f-string is cleaner.
                msg = f"[{time_sec:12.6f}] {line}\n"

                # Py3: The Python 2-specific version check and encode() call
                # are removed. gdb.write() in Python 3 handles unicode
                # strings correctly.
                gdb.write(msg)

            pos += length


# This registers the command with GDB.
LxDmesg()