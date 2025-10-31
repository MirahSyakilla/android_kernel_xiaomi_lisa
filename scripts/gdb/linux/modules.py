#
# gdb helper commands and functions for Linux kernel debugging
#
#  module tools
#
# Copyright (c) Siemens AG, 2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb

from linux import cpus, utils, lists


module_type = utils.CachedType("struct module")


def module_list():
    modules_head = utils.gdb_eval_or_none("modules")
    if modules_head is None:
        return

    module_ptr_type = module_type.get_type().pointer()

    for module in lists.list_for_each_entry(modules_head, module_ptr_type, "list"):
        yield module


def find_module_by_name(name):
    for module in module_list():
        if module['name'].string() == name:
            return module
    return None


class LxModule(gdb.Function):
    """Find module by name and return the module variable.

$lx_module("MODULE"): Given the name MODULE, iterate over all loaded modules
of the target and return that module variable which MODULE matches."""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx_module")

    def invoke(self, mod_name):
        mod_name_str = mod_name.string()
        module = find_module_by_name(mod_name_str)
        if module:
            return module.dereference()
        else:
            # Py3: Use f-string.
            raise gdb.GdbError(f"Unable to find MODULE {mod_name_str}")


LxModule()


class LxLsmod(gdb.Command):
    """List currently loaded modules."""

    _module_use_type = utils.CachedType("struct module_use")

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-lsmod", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        padding = "        " if utils.get_long_type().sizeof == 8 else ""
        gdb.write(f"Address{padding}    Module                  Size  Used by\n")

        for module in module_list():
            layout = module['core_layout']
            ref_count = int(module['refcnt']['counter']) - 1
            # Py3: Use f-string.
            gdb.write(f"{str(layout['base']).split()[0]} {module['name'].string():<19} "
                      f"{int(layout['size']):>8}  {ref_count}")

            t = self._module_use_type.get_type().pointer()
            first = True
            sources = module['source_list']
            for use in lists.list_for_each_entry(sources, t, "source_list"):
                separator = " " if first else ","
                gdb.write(f"{separator}{use['source']['name'].string()}")
                first = False

            gdb.write("\n")


LxLsmod()