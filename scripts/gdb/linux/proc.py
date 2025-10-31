#
# gdb helper commands and functions for Linux kernel debugging
#
#  Kernel proc information reader
#
# Copyright (c) 2016 Linaro Ltd
#
# Authors:
#  Kieran Bingham <kieran.bingham@linaro.org>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
from linux import constants
from linux import utils
from linux import tasks
from linux import lists
from struct import unpack, pack


class LxCmdLine(gdb.Command):
    """ Report the Linux Commandline used in the current kernel.
        Equivalent to cat /proc/cmdline on a running target"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-cmdline", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        gdb.write(gdb.parse_and_eval("saved_command_line").string() + "\n")


LxCmdLine()


class LxVersion(gdb.Command):
    """ Report the Linux Version of the current kernel.
        Equivalent to cat /proc/version on a running target"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-version", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        # linux_banner should contain a newline
        gdb.write(gdb.parse_and_eval("(char *)linux_banner").string())


LxVersion()


# Resource Structure Printers
#  /proc/iomem
#  /proc/ioports

def get_resources(resource, depth):
    while resource:
        yield resource, depth

        child = resource['child']
        if child:
            for res, deep in get_resources(child, depth + 1):
                yield res, deep

        resource = resource['sibling']


def show_lx_resources(resource_str):
        resource = gdb.parse_and_eval(resource_str)
        width = 4 if resource['end'] < 0x10000 else 8
        # Iterate straight to the first child
        for res, depth in get_resources(resource['child'], 0):
            start = int(res['start'])
            end = int(res['end'])
            # Py3: Use f-string for formatting.
            gdb.write(f"{' ' * depth * 2}"
                      f"{start:0{width}x}-{end:0{width}x} : "
                      f"{res['name'].string()}\n")


class LxIOMem(gdb.Command):
    """Identify the IO memory resource locations defined by the kernel

Equivalent to cat /proc/iomem on a running target"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-iomem", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        return show_lx_resources("iomem_resource")


LxIOMem()


class LxIOPorts(gdb.Command):
    """Identify the IO port resource locations defined by the kernel

Equivalent to cat /proc/ioports on a running target"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-ioports", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        return show_lx_resources("ioport_resource")


LxIOPorts()


# Mount namespace viewer
#  /proc/mounts

def info_opts(lst, opt):
    opts = ""
    for key, string in lst.items():
        if opt & key:
            opts += string
    return opts


FS_INFO = {constants.LX_SB_SYNCHRONOUS: ",sync",
           constants.LX_SB_MANDLOCK: ",mand",
           constants.LX_SB_DIRSYNC: ",dirsync",
           constants.LX_SB_NOATIME: ",noatime",
           constants.LX_SB_NODIRATIME: ",nodiratime"}

MNT_INFO = {constants.LX_MNT_NOSUID: ",nosuid",
            constants.LX_MNT_NODEV: ",nodev",
            constants.LX_MNT_NOEXEC: ",noexec",
            constants.LX_MNT_NOATIME: ",noatime",
            constants.LX_MNT_NODIRATIME: ",nodiratime",
            constants.LX_MNT_RELATIME: ",relatime"}

mount_type = utils.CachedType("struct mount")
mount_ptr_type = mount_type.get_type().pointer()


class LxMounts(gdb.Command):
    """Report the VFS mounts of the current process namespace.

Equivalent to cat /proc/mounts on a running target
An integer value can be supplied to display the mount
values of that process namespace"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-mounts", gdb.COMMAND_DATA)

    # Equivalent to proc_namespace.c:show_vfsmnt
    # However, that has the ability to call into s_op functions
    # whereas we cannot and must make do with the information we can obtain.
    def invoke(self, arg, from_tty):
        argv = gdb.string_to_argv(arg)
        if len(argv) >= 1:
            try:
                pid = int(argv[0])
            except ValueError:
                raise gdb.GdbError("Provide a PID as integer value")
        else:
            pid = 1

        task = tasks.get_task_by_pid(pid)
        if not task:
            raise gdb.GdbError(f"Couldn't find a process with PID {pid}")

        namespace = task['nsproxy']['mnt_ns']
        if not namespace:
            raise gdb.GdbError("No namespace for current process")

        for vfs in lists.list_for_each_entry(namespace['list'],
                                             mount_ptr_type, "mnt_list"):
            devname = vfs['mnt_devname'].string() or "none"

            pathname = ""
            parent = vfs
            while True:
                mntpoint = parent['mnt_mountpoint']
                pathname = utils.dentry_name(mntpoint) + pathname
                if (parent.address == parent['mnt_parent'].address):
                    break
                parent = parent['mnt_parent']

            if not pathname:
                pathname = "/"

            superblock = vfs['mnt']['mnt_sb']
            fstype = superblock['s_type']['name'].string()
            s_flags = int(superblock['s_flags'])
            m_flags = int(vfs['mnt']['mnt_flags'])
            rd = "ro" if (s_flags & constants.LX_SB_RDONLY) else "rw"

            gdb.write(
                f"{devname} {pathname} {fstype} {rd}"
                f"{info_opts(FS_INFO, s_flags)}"
                f"{info_opts(MNT_INFO, m_flags)} 0 0\n"
            )


LxMounts()


class LxFdtDump(gdb.Command):
    """Output Flattened Device Tree header and dump FDT blob to the filename
       specified as the command argument. Equivalent to
       'cat /proc/fdt > fdtdump.dtb' on a running target"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-fdtdump", gdb.COMMAND_DATA,
                         gdb.COMPLETE_FILENAME)

    def fdthdr_to_cpu(self, fdt_header):
        fdt_header_be = ">IIIIIII"
        fdt_header_le = "<IIIIIII"

        if utils.get_target_endianness() == utils.LITTLE_ENDIAN:
            output_fmt = fdt_header_le
        else:
            output_fmt = fdt_header_be

        return unpack(output_fmt, pack(fdt_header_be,
                                       int(fdt_header['magic']),
                                       int(fdt_header['totalsize']),
                                       int(fdt_header['off_dt_struct']),
                                       int(fdt_header['off_dt_strings']),
                                       int(fdt_header['off_mem_rsvmap']),
                                       int(fdt_header['version']),
                                       int(fdt_header['last_comp_version'])))

    def invoke(self, arg, from_tty):
        if not constants.LX_CONFIG_OF:
            raise gdb.GdbError("Kernel not compiled with CONFIG_OF\n")

        filename = arg if arg else "fdtdump.dtb"

        py_fdt_header_ptr = gdb.parse_and_eval(
            "(const struct fdt_header *) initial_boot_params")
        py_fdt_header = py_fdt_header_ptr.dereference()

        fdt_header = self.fdthdr_to_cpu(py_fdt_header)

        if fdt_header[0] != constants.LX_OF_DT_HEADER:
            raise gdb.GdbError("No flattened device tree magic found\n")

        gdb.write(f"fdt_magic:         0x{fdt_header[0]:02X}\n")
        gdb.write(f"fdt_totalsize:     0x{fdt_header[1]:02X}\n")
        gdb.write(f"off_dt_struct:     0x{fdt_header[2]:02X}\n")
        gdb.write(f"off_dt_strings:    0x{fdt_header[3]:02X}\n")
        gdb.write(f"off_mem_rsvmap:    0x{fdt_header[4]:02X}\n")
        gdb.write(f"version:           {fdt_header[5]}\n")
        gdb.write(f"last_comp_version: {fdt_header[6]}\n")

        inf = gdb.inferiors()[0]
        fdt_buf = utils.read_memoryview(inf, py_fdt_header_ptr,
                                        fdt_header[1]).tobytes()

        try:
            with open(filename, 'wb') as f:
                f.write(fdt_buf)
        except IOError:
            raise gdb.GdbError(f"Could not open file '{filename}' to dump fdt")

        gdb.write(f"Dumped fdt blob to {filename}\n")


LxFdtDump()