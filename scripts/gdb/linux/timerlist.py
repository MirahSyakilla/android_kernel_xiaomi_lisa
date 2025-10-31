# SPDX-License-Identifier: GPL-2.0
#
# Copyright 2019 Google LLC.

import binascii
import gdb

from linux import constants
from linux import cpus
from linux import rbtree
from linux import utils

timerqueue_node_type = utils.CachedType("struct timerqueue_node").get_type()
hrtimer_type = utils.CachedType("struct hrtimer").get_type()


def ktime_get():
    """Returns the current time, but not very accurately

    We can't read the hardware timer itself to add any nanoseconds
    that need to be added since we last stored the time in the
    timekeeper. But this is probably good enough for debug purposes."""
    tk_core = gdb.parse_and_eval("&tk_core")

    return tk_core['timekeeper']['tkr_mono']['base']


def print_timer(rb_node, idx):
    timerqueue = utils.container_of(rb_node, timerqueue_node_type.pointer(),
                                    "node")
    timer = utils.container_of(timerqueue, hrtimer_type.pointer(), "node")

    function = str(timer['function']).split(" ")[1].strip("<>")
    softexpires = timer['_softexpires']
    expires = timer['node']['expires']
    now = ktime_get()

    # Py3: Use f-strings for formatting.
    text = f" #{idx}: <{timer}>, {function}, "
    text += f"S:{int(timer['state']):02x}\n"
    text += (f" # expires at {softexpires}-{expires} nsecs [in {softexpires - now} "
             f"to {expires - now} nsecs]\n")
    return text


def print_active_timers(base):
    curr = base['active']['next']['node']
    curr = curr.address.cast(rbtree.rb_node_type.get_type().pointer())
    idx = 0
    while curr:
        yield print_timer(curr, idx)
        curr = rbtree.rb_next(curr)
        idx += 1


def print_base(base):
    # Py3: Use f-strings for formatting.
    text = f" .base:       {base.address}\n"
    text += f" .index:      {base['index']}\n"
    text += f" .resolution: {constants.LX_hrtimer_resolution} nsecs\n"
    text += f" .get_time:   {base['get_time']}\n"
    if constants.LX_CONFIG_HIGH_RES_TIMERS:
        text += f"  .offset:     {base['offset']} nsecs\n"
    text += "active timers:\n"
    text += "".join([x for x in print_active_timers(base)])
    return text


def print_cpu(hrtimer_bases, cpu, max_clock_bases):
    cpu_base = cpus.per_cpu(hrtimer_bases, cpu)
    jiffies = gdb.parse_and_eval("jiffies_64")
    tick_sched_ptr = gdb.parse_and_eval("&tick_cpu_sched")
    ts = cpus.per_cpu(tick_sched_ptr, cpu)

    text = f"cpu: {cpu}\n"
    for i in range(max_clock_bases):
        text += f" clock {i}:\n"
        text += print_base(cpu_base['clock_base'][i])

        if constants.LX_CONFIG_HIGH_RES_TIMERS:
            fmts = [("  .{}   : {} nsecs", 'expires_next'),
                    ("  .{}    : {}", 'hres_active'),
                    ("  .{}      : {}", 'nr_events'),
                    ("  .{}     : {}", 'nr_retries'),
                    ("  .{}       : {}", 'nr_hangs'),
                    ("  .{}  : {}", 'max_hang_time')]
            # Py3: Use f-strings for complex formatting.
            text += "\n".join([s.format(f, cpu_base[f]) for s, f in fmts])
            text += "\n"

        if constants.LX_CONFIG_TICK_ONESHOT:
            fmts = [("  .{}      : {}", 'nohz_mode'),
                    ("  .{}      : {} nsecs", 'last_tick'),
                    ("  .{}   : {}", 'tick_stopped'),
                    ("  .{}   : {}", 'idle_jiffies'),
                    ("  .{}     : {}", 'idle_calls'),
                    ("  .{}    : {}", 'idle_sleeps'),
                    ("  .{} : {} nsecs", 'idle_entrytime'),
                    ("  .{}  : {} nsecs", 'idle_waketime'),
                    ("  .{}  : {} nsecs", 'idle_exittime'),
                    ("  .{} : {} nsecs", 'idle_sleeptime'),
                    ("  .{}: {} nsecs", 'iowait_sleeptime'),
                    ("  .{}   : {}", 'last_jiffies'),
                    ("  .{}     : {}", 'next_timer'),
                    ("  .{}   : {} nsecs", 'idle_expires')]
            text += "\n".join([s.format(f, ts[f]) for s, f in fmts])
            text += f"\njiffies: {jiffies}\n"
        text += "\n"
    return text


def print_tickdevice(td, cpu):
    dev = td['evtdev']
    text = f"Tick Device: mode:     {td['mode']}\n"
    if cpu < 0:
        text += "Broadcast device\n"
    else:
        text += f"Per CPU device: {cpu}\n"

    text += "Clock Event Device: "
    if dev == 0:
        text += "<NULL>\n"
        return text

    text += f"{dev['name'].string()}\n"
    text += f" max_delta_ns:   {dev['max_delta_ns']}\n"
    text += f" min_delta_ns:   {dev['min_delta_ns']}\n"
    text += f" mult:           {dev['mult']}\n"
    text += f" shift:          {dev['shift']}\n"
    text += f" mode:           {dev['state_use_accessors']}\n"
    text += f" next_event:     {dev['next_event']} nsecs\n"
    text += f" set_next_event: {dev['set_next_event']}\n"

    members = [('set_state_shutdown', " shutdown: {}\n"),
               ('set_state_periodic', " periodic: {}\n"),
               ('set_state_oneshot', " oneshot:  {}\n"),
               ('set_state_oneshot_stopped', " oneshot stopped: {}\n"),
               ('tick_resume', " resume:   {}\n")]
    for member, fmt in members:
        if dev[member]:
            text += fmt.format(dev[member])

    text += f" event_handler:  {dev['event_handler']}\n"
    text += f" retries:        {dev['retries']}\n"
    return text


def pr_cpumask(mask):
    nr_cpu_ids = 1
    if constants.LX_NR_CPUS > 1:
        nr_cpu_ids = gdb.parse_and_eval("nr_cpu_ids")

    inf = gdb.inferiors()[0]
    bits = mask['bits']
    # Py3: Use integer division //
    num_bytes = (nr_cpu_ids + 7) // 8
    buf_bytes = utils.read_memoryview(inf, bits, num_bytes).tobytes()
    # Py3: b2a_hex returns bytes, so decode it to a string.
    buf = binascii.b2a_hex(buf_bytes).decode('ascii')

    chunks = []
    i = num_bytes
    while i > 0:
        i -= 1
        start = i * 2
        end = start + 2
        chunks.append(buf[start:end])
        if i != 0 and i % 4 == 0:
            chunks.append(',')

    extra = nr_cpu_ids % 8
    if 0 < extra <= 4:
        chunks[0] = chunks[0][0]  # Cut off the first 0

    return "".join(chunks)


class LxTimerList(gdb.Command):
    """Print /proc/timer_list"""

    def __init__(self):
        # Py3: Use modern, argument-less super().
        super().__init__("lx-timerlist", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        hrtimer_bases = gdb.parse_and_eval("&hrtimer_bases")
        max_clock_bases = gdb.parse_and_eval("HRTIMER_MAX_CLOCK_BASES")

        text = "Timer List Version: gdb scripts\n"
        text += f"HRTIMER_MAX_CLOCK_BASES: {max_clock_bases}\n"
        text += f"now at {ktime_get()} nsecs\n"

        for cpu in cpus.each_online_cpu():
            text += print_cpu(hrtimer_bases, cpu, max_clock_bases)

        if constants.LX_CONFIG_GENERIC_CLOCKEVENTS:
            if constants.LX_CONFIG_GENERIC_CLOCKEVENTS_BROADCAST:
                bc_dev = gdb.parse_and_eval("&tick_broadcast_device")
                text += print_tickdevice(bc_dev, -1)
                text += "\n"
                mask = gdb.parse_and_eval("tick_broadcast_mask")
                mask_str = pr_cpumask(mask)
                text += f"tick_broadcast_mask: {mask_str}\n"
                if constants.LX_CONFIG_TICK_ONESHOT:
                    mask = gdb.parse_and_eval("tick_broadcast_oneshot_mask")
                    mask_str = pr_cpumask(mask)
                    text += f"tick_broadcast_oneshot_mask: {mask_str}\n"
                text += "\n"

            tick_cpu_devices = gdb.parse_and_eval("&tick_cpu_device")
            for cpu in cpus.each_online_cpu():
                tick_dev = cpus.per_cpu(tick_cpu_devices, cpu)
                text += print_tickdevice(tick_dev, cpu)
                text += "\n"

        gdb.write(text)


LxTimerList()