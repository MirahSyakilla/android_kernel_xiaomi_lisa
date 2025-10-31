# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) NXP 2019

import gdb
# Py3: sys module is not used, can be removed.
# import sys

from linux import utils, lists, constants

clk_core_type = utils.CachedType("struct clk_core")


def clk_core_for_each_child(hlist_head):
    return lists.hlist_for_each_entry(hlist_head,
            clk_core_type.get_type().pointer(), "child_node")


class LxClkSummary(gdb.Command):
    """Print clk tree summary

Output is a subset of /sys/kernel/debug/clk/clk_summary

No calls are made during printing, instead a (c) if printed after values which
are cached and potentially out of date"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-clk-summary", gdb.COMMAND_DATA)

    def show_subtree(self, clk, level):
        # Py3: Use f-string for complex formatting.
        padding = " " * (level * 3 + 1)
        name = clk['name'].string()
        cached_flag = '(c)' if clk['flags'] & constants.LX_CLK_GET_RATE_NOCACHE else '   '
        gdb.write(
            f"{padding}{name:<{30 - level * 3}} "
            f"{int(clk['enable_count']):7d} "
            f"{int(clk['prepare_count']):8d} "
            f"{int(clk['protect_count']):8d} "
            f"{int(clk['rate']):11d}"
            f"{cached_flag}\n"
        )

        for child in clk_core_for_each_child(clk['children']):
            self.show_subtree(child, level + 1)

    def invoke(self, arg, from_tty):
        if utils.gdb_eval_or_none("clk_root_list") is None:
            raise gdb.GdbError("No clocks registered")
        gdb.write("                                 enable  prepare  protect               \n")
        gdb.write("   clock                          count    count    count        rate   \n")
        gdb.write("------------------------------------------------------------------------\n")
        for clk in clk_core_for_each_child(gdb.parse_and_eval("clk_root_list")):
            self.show_subtree(clk, 0)
        for clk in clk_core_for_each_child(gdb.parse_and_eval("clk_orphan_list")):
            self.show_subtree(clk, 0)


LxClkSummary()


class LxClkCoreLookup(gdb.Function):
    """Find struct clk_core by name"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx_clk_core_lookup")

    def lookup_hlist(self, hlist_head, name):
        for child in clk_core_for_each_child(hlist_head):
            if child['name'].string() == name:
                return child
            result = self.lookup_hlist(child['children'], name)
            if result:
                return result

    def invoke(self, name):
        name_str = name.string()
        return (self.lookup_hlist(gdb.parse_and_eval("clk_root_list"), name_str) or
                self.lookup_hlist(gdb.parse_and_eval("clk_orphan_list"), name_str))


LxClkCoreLookup()