/*
 * arch/arm64/kernel/topology.c
 *
 * Copyright (C) 2011,2013,2014 Linaro Limited.
 *
 * Based on the arm32 version written by Vincent Guittot in turn based on
 * arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/acpi.h>
#include <linux/arch_topology.h>
#include <linux/cacheinfo.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/percpu.h>

#include <clocksource/arm_arch_timer.h>

#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/sysreg.h>
#include <asm/topology.h>

#ifdef CONFIG_ARM64_AMU_EXTN
#define read_corecnt()	read_sysreg_s(SYS_AMEVCNTR0_CORE_EL0)
#define read_constcnt()	read_sysreg_s(SYS_AMEVCNTR0_CONST_EL0)
#else
#define read_corecnt()	(0UL)
#define read_constcnt()	(0UL)
#endif

#ifdef CONFIG_ACPI
static bool __init acpi_cpu_is_threaded(int cpu)
{
	int is_threaded = acpi_pptt_cpu_is_thread(cpu);

	/*
	 * if the PPTT doesn't have thread information, assume a homogeneous
	 * machine and return the current CPU's thread state.
	 */
	if (is_threaded < 0)
		is_threaded = read_cpuid_mpidr() & MPIDR_MT_BITMASK;

	return !!is_threaded;
}

/*
 * Propagate the topology information of the processor_topology_node tree to the
 * cpu_topology array.
 */
int __init parse_acpi_topology(void)
{
	int cpu, topology_id;

	if (acpi_disabled)
		return 0;

	for_each_possible_cpu(cpu) {
		int i, cache_id;

		topology_id = find_acpi_cpu_topology(cpu, 0);
		if (topology_id < 0)
			return topology_id;

		if (acpi_cpu_is_threaded(cpu)) {
			cpu_topology[cpu].thread_id = topology_id;
			topology_id = find_acpi_cpu_topology(cpu, 1);
			cpu_topology[cpu].core_id   = topology_id;
		} else {
			cpu_topology[cpu].thread_id  = -1;
			cpu_topology[cpu].core_id    = topology_id;
		}
		topology_id = find_acpi_cpu_topology_package(cpu);
		cpu_topology[cpu].package_id = topology_id;

		i = acpi_find_last_cache_level(cpu);

		if (i > 0) {
			/*
			 * this is the only part of cpu_topology that has
			 * a direct relationship with the cache topology
			 */
			cache_id = find_acpi_cpu_cache_topology(cpu, i);
			if (cache_id > 0)
				cpu_topology[cpu].llc_id = cache_id;
		}
	}

	return 0;
}
#endif

#ifdef CONFIG_ARM64_AMU_EXTN

#undef pr_fmt
#define pr_fmt(fmt) "AMU: " fmt

static DEFINE_PER_CPU_READ_MOSTLY(unsigned long, arch_max_freq_scale);
static DEFINE_PER_CPU(u64, arch_const_cycles_prev);
static DEFINE_PER_CPU(u64, arch_core_cycles_prev);
static cpumask_var_t amu_fie_cpus;

void update_freq_counters_refs(void)
{
	this_cpu_write(arch_core_cycles_prev, read_corecnt());
	this_cpu_write(arch_const_cycles_prev, read_constcnt());
}

static inline bool freq_counters_valid(int cpu)
{
	if (cpu >= nr_cpu_ids || !cpumask_test_cpu(cpu, cpu_present_mask))
		return false;

	if (!cpu_has_amu_feat(cpu)) {
		pr_debug("CPU%d: counters are not supported.\n", cpu);
		return false;
	}

	if (unlikely(!per_cpu(arch_const_cycles_prev, cpu) ||
		     !per_cpu(arch_core_cycles_prev, cpu))) {
		pr_debug("CPU%d: cycle counters are not enabled.\n", cpu);
		return false;
	}

	return true;
}

static int freq_inv_set_max_ratio(int cpu, u64 max_rate, u64 ref_rate)
{
	u64 ratio;

	if (unlikely(!max_rate || !ref_rate)) {
		pr_debug("CPU%d: invalid maximum or reference frequency.\n",
			 cpu);
		return -EINVAL;
	}

	/*
	 * Pre-compute the fixed ratio between the frequency of the constant
	 * reference counter and the maximum frequency of the CPU.
	 *
	 *			    ref_rate
	 * arch_max_freq_scale =   ---------- * SCHED_CAPACITY_SCALE^2
	 *			    max_rate
	 */
	ratio = ref_rate << (2 * SCHED_CAPACITY_SHIFT);
	ratio = div64_u64(ratio, max_rate);
	if (!ratio) {
		WARN_ONCE(1, "Reference frequency too low.\n");
		return -EINVAL;
	}

	per_cpu(arch_max_freq_scale, cpu) = (unsigned long)ratio;

	return 0;
}

static void amu_scale_freq_tick(void)
{
	u64 prev_core_cnt, prev_const_cnt;
	u64 core_cnt, const_cnt, scale;

	prev_const_cnt = this_cpu_read(arch_const_cycles_prev);
	prev_core_cnt = this_cpu_read(arch_core_cycles_prev);

	update_freq_counters_refs();

	const_cnt = this_cpu_read(arch_const_cycles_prev);
	core_cnt = this_cpu_read(arch_core_cycles_prev);

	if (unlikely(core_cnt <= prev_core_cnt ||
		     const_cnt <= prev_const_cnt))
		return;

	/*
	 *	    delta(core)   arch_max_freq_scale
	 * scale = ------------ * -------------------
	 *	    delta(const)   SCHED_CAPACITY_SCALE
	 */
	scale = core_cnt - prev_core_cnt;
	scale *= this_cpu_read(arch_max_freq_scale);
	scale = div64_u64(scale >> SCHED_CAPACITY_SHIFT,
			  const_cnt - prev_const_cnt);

	scale = min_t(unsigned long, scale, SCHED_CAPACITY_SCALE);
	this_cpu_write(freq_scale, (unsigned long)scale);
}

static struct scale_freq_data amu_sfd = {
	.source = SCALE_FREQ_SOURCE_ARCH,
	.set_freq_scale = amu_scale_freq_tick,
};

static void amu_fie_setup(const struct cpumask *cpus, unsigned int max_freq)
{
	int cpu;

	if (unlikely(cpumask_subset(cpus, amu_fie_cpus)))
		return;

	for_each_cpu(cpu, cpus) {
		if (!freq_counters_valid(cpu) ||
		    freq_inv_set_max_ratio(cpu,
					   (u64)max_freq * 1000,
					   arch_timer_get_rate()))
			return;
	}

	cpumask_or(amu_fie_cpus, amu_fie_cpus, cpus);
	topology_set_scale_freq_source(&amu_sfd, amu_fie_cpus);

	pr_debug("CPUs[%*pbl]: counters will be used for FIE.\n",
		 cpumask_pr_args(cpus));
}

static int init_amu_fie_callback(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct cpufreq_policy *policy = data;

	if (val == CPUFREQ_CREATE_POLICY)
		amu_fie_setup(policy->related_cpus, policy->cpuinfo.max_freq);

	return 0;
}

static struct notifier_block init_amu_fie_notifier = {
	.notifier_call = init_amu_fie_callback,
};

static int __init init_amu_fie(void)
{
	int ret;

	if (!zalloc_cpumask_var(&amu_fie_cpus, GFP_KERNEL))
		return -ENOMEM;

	ret = cpufreq_register_notifier(&init_amu_fie_notifier,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		free_cpumask_var(amu_fie_cpus);

	return ret;
}
core_initcall(init_amu_fie);
#endif /* CONFIG_ARM64_AMU_EXTN */
