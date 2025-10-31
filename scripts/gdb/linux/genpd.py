# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) NXP 2019

import gdb
# Py3: sys module is not used.
# import sys

from linux.utils import CachedType
from linux.lists import list_for_each_entry

generic_pm_domain_type = CachedType('struct generic_pm_domain')
pm_domain_data_type = CachedType('struct pm_domain_data')
device_link_type = CachedType('struct device_link')


def kobject_get_path(kobj):
    path = kobj['name'].string()
    parent = kobj['parent']
    if parent:
        path = kobject_get_path(parent) + '/' + path
    return path


def rtpm_status_str(dev):
    if dev['power']['runtime_error']:
        return 'error'
    if dev['power']['disable_depth']:
        return 'unsupported'
    _RPM_STATUS_LOOKUP = [
        "active",
        "resuming",
        "suspended",
        "suspending"
    ]
    return _RPM_STATUS_LOOKUP[int(dev['power']['runtime_status'])]


class LxGenPDSummary(gdb.Command):
    '''Print genpd summary

Output is similar to /sys/kernel/debug/pm_genpd/pm_genpd_summary'''

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__('lx-genpd-summary', gdb.COMMAND_DATA)

    def summary_one(self, genpd):
        if genpd['status'] == 0:
            status_string = 'on'
        else:
            status_string = f"off-{genpd['state_idx']}"

        slave_names = []
        for link in list_for_each_entry(
                genpd['master_links'],
                device_link_type.get_type().pointer(),
                'master_node'):
            # Py3: Fixed typo from .apend to .append
            slave_names.append(link['slave']['name'].string())

        # Py3: Use f-string for formatting.
        gdb.write(f"{genpd['name'].string():<30}  {status_string:<15} {', '.join(slave_names)}\n")

        # Print devices in domain
        for pm_data in list_for_each_entry(genpd['dev_list'],
                        pm_domain_data_type.get_type().pointer(),
                        'list_node'):
            dev = pm_data['dev']
            kobj_path = kobject_get_path(dev['kobj'])
            gdb.write(f"    {kobj_path:<50}  {rtpm_status_str(dev)}\n")

    def invoke(self, arg, from_tty):
        gdb.write('domain                          status          slaves\n')
        gdb.write('    /device                                             runtime status\n')
        gdb.write('----------------------------------------------------------------------\n')
        for genpd in list_for_each_entry(
                gdb.parse_and_eval('&gpd_list'),
                generic_pm_domain_type.get_type().pointer(),
                'gpd_list_node'):
            self.summary_one(genpd)


LxGenPDSummary()