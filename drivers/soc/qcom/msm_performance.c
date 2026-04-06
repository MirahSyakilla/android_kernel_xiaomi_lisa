// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <trace/events/power.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/kthread.h>
#ifdef CONFIG_SCHED_WALT
#include <linux/sched/core_ctl.h>
#endif
#include <soc/qcom/msm_performance.h>
#include <linux/spinlock.h>
#include <linux/circ_buf.h>
#include <linux/ktime.h>
#include <linux/perf_event.h>
#include <linux/errno.h>
#include <linux/topology.h>
#include <linux/scmi_protocol.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>

#define POLL_INT 25
#define NODE_NAME_MAX_CHARS 16

#define QUEUE_POOL_SIZE 512 /*2^8 always keep in 2^x */
#define INST_EV 0x08 /* 0th event*/
#define CYC_EV 0x11 /* 1st event*/
#define INIT "Init"
#define CPU_CYCLE_THRESHOLD 650000

#ifdef CONFIG_MSM_PERFORMANCE_QGKI
static DEFINE_PER_CPU(bool, cpu_is_idle);
static DEFINE_PER_CPU(bool, cpu_is_hp);
static DEFINE_MUTEX(perfevent_lock);
#endif

enum event_idx {
	INST_EVENT,
	CYC_EVENT,
	NO_OF_EVENT
};

enum cpu_clusters {
	MIN = 0,
	MID = 1,
	MAX = 2,
	CLUSTER_MAX
};

/* To handle cpufreq min/max request */
struct cpu_status {
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, msm_perf_cpu_stats);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_min);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_max);

static cpumask_var_t limit_mask_min;
static cpumask_var_t limit_mask_max;

#ifdef CONFIG_MSM_PERFORMANCE_QGKI
static DECLARE_COMPLETION(gfx_evt_arrival);

struct gpu_data {
	pid_t pid;
	int ctx_id;
	unsigned int timestamp;
	ktime_t arrive_ts;
	int evt_typ;
};

static struct gpu_data gpu_circ_buff[QUEUE_POOL_SIZE];

struct queue_indicies {
	int head;
	int tail;
};
static struct queue_indicies curr_pos;

static DEFINE_SPINLOCK(gfx_circ_buff_lock);

static struct event_data {
	struct perf_event *pevent;
	u64 prev_count;
	u64 cur_delta;
	u64 cached_total_count;
} pmu_events[NO_OF_EVENT][NR_CPUS];

struct events {
	spinlock_t cpu_hotplug_lock;
	bool cpu_hotplug;
	bool init_success;
};
static struct events events_group;
static struct task_struct *events_notify_thread;

static unsigned int aggr_big_nr;
static unsigned int aggr_top_load;
static unsigned int top_load[CLUSTER_MAX];
static unsigned int curr_cap[CLUSTER_MAX];
static bool max_cap_cpus[NR_CPUS];
static unsigned long perf_cpu_capacity[NR_CPUS];
static DEFINE_PER_CPU(u8, perf_cluster_id);
static atomic_t game_status_pid;
#ifndef CONFIG_SCHED_WALT
static void msm_perf_reset_compat_accumulators(void);
#endif
#endif
static bool ready_for_freq_updates;

static int freq_qos_request_init(void)
{
	unsigned int cpu;
	int ret;

	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EAGAIN;
			goto cleanup;
		}
		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		req = &per_cpu(qos_req_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(msm_perf_cpu_stats, cpu).max = UINT_MAX;
		req = &per_cpu(qos_req_max, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add max freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		cpufreq_cpu_put(policy);
	}
	return 0;

cleanup:
	for_each_present_cpu(cpu) {
		req = &per_cpu(qos_req_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);


		req = &per_cpu(qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		per_cpu(msm_perf_cpu_stats, cpu).max = UINT_MAX;
	}
	return ret;
}

/*******************************sysfs start************************************/
static int set_cpu_min_freq(const char *buf, const struct kernel_param *kp)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpu_status *i_cpu_stats;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	int ret = 0;

	if (!ready_for_freq_updates) {
		ret = freq_qos_request_init();
		if (ret) {
			pr_err("%s: Failed to init qos requests policy for ret=%d\n",
				__func__, ret);
			return ret;
		}
		ready_for_freq_updates = true;
	}

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_mask_min);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= nr_cpu_ids)
			break;

		if (cpu_possible(cpu)) {
			i_cpu_stats = &per_cpu(msm_perf_cpu_stats, cpu);

			i_cpu_stats->min = val;
			cpumask_set_cpu(cpu, limit_mask_min);
		}

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	/*
	 * Since on synchronous systems policy is shared amongst multiple
	 * CPUs only one CPU needs to be updated for the limit to be
	 * reflected for the entire cluster. We can avoid updating the policy
	 * of other CPUs in the cluster once it is done for at least one CPU
	 * in the cluster
	 */
	get_online_cpus();
	for_each_cpu(i, limit_mask_min) {
		i_cpu_stats = &per_cpu(msm_perf_cpu_stats, i);

		if (cpufreq_get_policy(&policy, i))
			continue;

		if (cpu_online(i)) {
			req = &per_cpu(qos_req_min, i);
			if (freq_qos_update_request(req, i_cpu_stats->min) < 0)
				break;
		}

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, limit_mask_min);
	}
	put_online_cpus();

	return 0;
}

static int get_cpu_min_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;

	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu,
				per_cpu(msm_perf_cpu_stats, cpu).min);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_cpu_min_freq = {
	.set = set_cpu_min_freq,
	.get = get_cpu_min_freq,
};
module_param_cb(cpu_min_freq, &param_ops_cpu_min_freq, NULL, 0644);

static int set_cpu_max_freq(const char *buf, const struct kernel_param *kp)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpu_status *i_cpu_stats;
	struct cpufreq_policy policy;
	struct freq_qos_request *req;
	int ret = 0;

	if (!ready_for_freq_updates) {
		ret = freq_qos_request_init();
		if (ret) {
			pr_err("%s: Failed to init qos requests policy for ret=%d\n",
				__func__, ret);
			return ret;
		}
		ready_for_freq_updates = true;
	}

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_mask_max);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= nr_cpu_ids)
			break;

		if (cpu_possible(cpu)) {
			i_cpu_stats = &per_cpu(msm_perf_cpu_stats, cpu);

			i_cpu_stats->max = val;
			cpumask_set_cpu(cpu, limit_mask_max);
		}

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	get_online_cpus();
	for_each_cpu(i, limit_mask_max) {
		i_cpu_stats = &per_cpu(msm_perf_cpu_stats, i);
		if (cpufreq_get_policy(&policy, i))
			continue;

		if (cpu_online(i)) {
			req = &per_cpu(qos_req_max, i);
			if (freq_qos_update_request(req, i_cpu_stats->max) < 0)
				break;
		}

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, limit_mask_max);
	}
	put_online_cpus();

	return 0;
}

static int get_cpu_max_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;

	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu,
				per_cpu(msm_perf_cpu_stats, cpu).max);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_cpu_max_freq = {
	.set = set_cpu_max_freq,
	.get = get_cpu_max_freq,
};
module_param_cb(cpu_max_freq, &param_ops_cpu_max_freq, NULL, 0644);
#ifdef CONFIG_MSM_PERFORMANCE_QGKI
static struct kobject *events_kobj;

static ssize_t show_cpu_hotplug(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n");
}
static struct kobj_attribute cpu_hotplug_attr =
__ATTR(cpu_hotplug, 0444, show_cpu_hotplug, NULL);

static struct attribute *events_attrs[] = {
	&cpu_hotplug_attr.attr,
	NULL,
};

static struct attribute_group events_attr_group = {
	.attrs = events_attrs,
};

static ssize_t show_perf_gfx_evts(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   char *buf)
{
	struct queue_indicies updated_pos;
	unsigned long flags;
	ssize_t retval = 0;
	int idx = 0, size, act_idx, ret = -1;

	ret = wait_for_completion_interruptible(&gfx_evt_arrival);
	if (ret)
		return 0;
	spin_lock_irqsave(&gfx_circ_buff_lock, flags);
	updated_pos.head = curr_pos.head;
	updated_pos.tail = curr_pos.tail;
	size = CIRC_CNT(updated_pos.head, updated_pos.tail, QUEUE_POOL_SIZE);
	curr_pos.tail = (curr_pos.tail + size) % QUEUE_POOL_SIZE;
	spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);

	for (idx = 0; idx < size; idx++) {
		act_idx = (updated_pos.tail + idx) % QUEUE_POOL_SIZE;
		retval += scnprintf(buf + retval, PAGE_SIZE - retval,
			  "%d %d %u %d %lu :",
			  gpu_circ_buff[act_idx].pid,
			  gpu_circ_buff[act_idx].ctx_id,
			  gpu_circ_buff[act_idx].timestamp,
			  gpu_circ_buff[act_idx].evt_typ,
			  ktime_to_us(gpu_circ_buff[act_idx].arrive_ts));
		if (retval >= PAGE_SIZE) {
			pr_err("msm_perf:data limit exceed\n");
			break;
		}
	}
	return retval;
}

static struct kobj_attribute gfx_event_info_attr =
__ATTR(gfx_evt, 0444, show_perf_gfx_evts, NULL);

static ssize_t show_big_nr(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(aggr_big_nr));
}

static struct kobj_attribute big_nr_attr =
__ATTR(aggr_big_nr, 0444, show_big_nr, NULL);

static ssize_t show_top_load(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(aggr_top_load));
}

static struct kobj_attribute top_load_attr =
__ATTR(aggr_top_load, 0444, show_top_load, NULL);


static ssize_t show_top_load_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u %u %u\n",
					READ_ONCE(top_load[MIN]),
					READ_ONCE(top_load[MID]),
					READ_ONCE(top_load[MAX]));
}

static struct kobj_attribute cluster_top_load_attr =
__ATTR(top_load_cluster, 0444, show_top_load_cluster, NULL);

static ssize_t show_curr_cap_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u %u %u\n",
					READ_ONCE(curr_cap[MIN]),
					READ_ONCE(curr_cap[MID]),
					READ_ONCE(curr_cap[MAX]));
}

static struct kobj_attribute cluster_curr_cap_attr =
__ATTR(curr_cap_cluster, 0444, show_curr_cap_cluster, NULL);

static struct attribute *notify_attrs[] = {
	&big_nr_attr.attr,
	&top_load_attr.attr,
	&cluster_top_load_attr.attr,
	&cluster_curr_cap_attr.attr,
	&gfx_event_info_attr.attr,
	NULL,
};

static struct attribute_group notify_attr_group = {
	.attrs = notify_attrs,
};
static struct kobject *notify_kobj;

/*******************************sysfs ends************************************/

/*****************PMU Data Collection*****************/
static struct perf_event_attr attr;
static void msm_perf_init_attr(void)
{
	memset(&attr, 0, sizeof(struct perf_event_attr));

	attr.type = PERF_TYPE_RAW;
	attr.size = sizeof(struct perf_event_attr);
	attr.pinned = 1;
}

static int set_event(struct event_data *ev, int cpu)
{
	struct perf_event *pevent;

	pevent = perf_event_create_kernel_counter(&attr,
				cpu, NULL, NULL, NULL);
	if (IS_ERR(pevent)) {
		pr_err("msm_perf: %s failed, eventId:0x%x, cpu:%d, error code:%ld\n",
				__func__, attr.config, cpu, PTR_ERR(pevent));
		return PTR_ERR(pevent);
	}
	ev->pevent = pevent;
	perf_event_enable(pevent);

	return 0;
}

static void free_pmu_counters(unsigned int cpu)
{
	int i = 0;

	for (i = 0; i < NO_OF_EVENT; i++) {
		pmu_events[i][cpu].prev_count = 0;
		pmu_events[i][cpu].cur_delta = 0;
		pmu_events[i][cpu].cached_total_count = 0;
		if (pmu_events[i][cpu].pevent) {
			perf_event_disable(pmu_events[i][cpu].pevent);
			perf_event_release_kernel(pmu_events[i][cpu].pevent);
			pmu_events[i][cpu].pevent = NULL;
		}
	}
}

static int init_pmu_counter(void)
{
	int cpu;
	unsigned long cpu_capacity[NR_CPUS] = {0};
	unsigned long min_cpu_capacity = ULONG_MAX;
	unsigned long max_cpu_capacity = 0;
	int ret = 0;

	msm_perf_init_attr();

	/* Create events per CPU */
	for_each_possible_cpu(cpu) {
		/* create Instruction event */
		attr.config = INST_EV;
		ret = set_event(&pmu_events[INST_EVENT][cpu], cpu);
		if (ret < 0) {
			return ret;
		}
		/* create cycle event */
		attr.config = CYC_EV;
		ret = set_event(&pmu_events[CYC_EVENT][cpu], cpu);
		if (ret < 0) {
			free_pmu_counters(cpu);
			return ret;
		}
			/* find capacity per cpu */
			cpu_capacity[cpu] = arch_scale_cpu_capacity(cpu);
			perf_cpu_capacity[cpu] = cpu_capacity[cpu];
			if (cpu_capacity[cpu] < min_cpu_capacity)
				min_cpu_capacity = cpu_capacity[cpu];
		if (cpu_capacity[cpu] > max_cpu_capacity)
			max_cpu_capacity = cpu_capacity[cpu];
	}

	if (max_cpu_capacity == min_cpu_capacity) {
		for_each_possible_cpu(cpu)
			per_cpu(perf_cluster_id, cpu) = MIN;
	} else {
		unsigned long mid_cpu_capacity = max_cpu_capacity;

		for_each_possible_cpu(cpu) {
			if (cpu_capacity[cpu] > min_cpu_capacity &&
			    cpu_capacity[cpu] < mid_cpu_capacity)
				mid_cpu_capacity = cpu_capacity[cpu];
		}

		if (mid_cpu_capacity == max_cpu_capacity)
			mid_cpu_capacity = min_cpu_capacity;

		for_each_possible_cpu(cpu) {
			if (cpu_capacity[cpu] == max_cpu_capacity) {
				per_cpu(perf_cluster_id, cpu) = MAX;
			} else if (cpu_capacity[cpu] == min_cpu_capacity) {
				per_cpu(perf_cluster_id, cpu) = MIN;
			} else {
				per_cpu(perf_cluster_id, cpu) = MID;
			}
		}
	}

	/* determine cpu index for maximum capacity cpus */
	for_each_possible_cpu(cpu) {
		if (cpu_capacity[cpu] == max_cpu_capacity)
			max_cap_cpus[cpu] = true;
	}

	return 0;
}

static inline void msm_perf_read_event(struct event_data *event)
{
	u64 ev_count = 0;
	u64 total, enabled, running;

	mutex_lock(&perfevent_lock);
	if (!event->pevent) {
		mutex_unlock(&perfevent_lock);
		return;
	}

	if (!per_cpu(cpu_is_idle, event->pevent->cpu) &&
				!per_cpu(cpu_is_hp, event->pevent->cpu)) {
		total = perf_event_read_value(event->pevent, &enabled, &running);
		event->cached_total_count = total;
	} else {
		total = event->cached_total_count;
	}

	ev_count = total - event->prev_count;
	event->prev_count = total;
	event->cur_delta = ev_count;
	mutex_unlock(&perfevent_lock);
}

static int get_cpu_total_instruction(char *buf, const struct kernel_param *kp)
{
	u64 instruction = 0;
	u64 cycles = 0;
	u64 total_inst_big = 0;
	u64 total_inst_little = 0;
	u64 ipc_big = 0;
	u64 ipc_little = 0;
	int cnt = 0, cpu;

	for_each_possible_cpu(cpu) {
		/* Read Instruction event */
		msm_perf_read_event(&pmu_events[INST_EVENT][cpu]);
		/* Read Cycle event */
		msm_perf_read_event(&pmu_events[CYC_EVENT][cpu]);
		instruction = pmu_events[INST_EVENT][cpu].cur_delta;
		cycles = pmu_events[CYC_EVENT][cpu].cur_delta;
		/* collecting max inst and ipc for max cap and min cap cpus */
		if (max_cap_cpus[cpu]) {
			if (cycles && cycles >= CPU_CYCLE_THRESHOLD)
				ipc_big = max(ipc_big,
						((instruction*100)/cycles));
			total_inst_big += instruction;
		} else {
			if (cycles)
				ipc_little = max(ipc_little,
						((instruction*100)/cycles));
			total_inst_little += instruction;
		}
	}

	cnt += scnprintf(buf, PAGE_SIZE, "%llu:%llu:%llu:%llu\n",
			total_inst_big, ipc_big,
			total_inst_little, ipc_little);

	return cnt;
}

static const struct kernel_param_ops param_ops_cpu_total_instruction = {
	.set = NULL,
	.get = get_cpu_total_instruction,
};
module_param_cb(inst, &param_ops_cpu_total_instruction, NULL, 0444);


static int restart_events(unsigned int cpu, bool cpu_up)
{
	int ret = 0;

	msm_perf_init_attr();

	if (cpu_up) {
		/* create Instruction event */
		attr.config = INST_EV;
		ret = set_event(&pmu_events[INST_EVENT][cpu], cpu);
		if (ret < 0) {
			return ret;
		}
		/* create cycle event */
		attr.config = CYC_EV;
		ret = set_event(&pmu_events[CYC_EVENT][cpu], cpu);
		if (ret < 0) {
			free_pmu_counters(cpu);
			return ret;
		}
	} else {
		free_pmu_counters(cpu);
	}

	return 0;
}

static int hotplug_notify_down(unsigned int cpu)
{
	mutex_lock(&perfevent_lock);
	per_cpu(cpu_is_hp, cpu) = true;
	restart_events(cpu, false);
	mutex_unlock(&perfevent_lock);
#ifndef CONFIG_SCHED_WALT
	msm_perf_reset_compat_accumulators();
#endif

	return 0;
}

static int hotplug_notify_up(unsigned int cpu)
{
	unsigned long flags;

	mutex_lock(&perfevent_lock);
	restart_events(cpu, true);
	per_cpu(cpu_is_hp, cpu) = false;
	mutex_unlock(&perfevent_lock);
#ifndef CONFIG_SCHED_WALT
	perf_cpu_capacity[cpu] = arch_scale_cpu_capacity(cpu);
	max_cap_cpus[cpu] = (per_cpu(perf_cluster_id, cpu) == MAX);
	msm_perf_reset_compat_accumulators();
#endif

	if (events_group.init_success) {
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);
		events_group.cpu_hotplug = true;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);
		wake_up_process(events_notify_thread);
	}

	return 0;
}

static int msm_perf_idle_read_events(unsigned int cpu)
{
	int ret = 0, i;

	for (i = 0; i < NO_OF_EVENT; i++) {
		if (pmu_events[i][cpu].pevent)
			ret = perf_event_read_local(pmu_events[i][cpu].pevent,
					&pmu_events[i][cpu].cached_total_count, NULL, NULL);
	}

	return ret;
}

static int msm_perf_idle_notif(struct notifier_block *nb, unsigned long action,
							void *data)
{
	int ret = NOTIFY_OK;
	int cpu = smp_processor_id();

	switch (action) {
	case IDLE_START:
		__this_cpu_write(cpu_is_idle, true);
		if (!per_cpu(cpu_is_hp, cpu))
			ret = msm_perf_idle_read_events(cpu);
		break;
	case IDLE_END:
		__this_cpu_write(cpu_is_idle, false);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block msm_perf_event_idle_nb = {
	.notifier_call = msm_perf_idle_notif,
};

static int events_notify_userspace(void *data)
{
	unsigned long flags;
	bool notify_change;

	while (1) {

		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);

		if (!events_group.cpu_hotplug) {
			spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock),
									flags);

			schedule();
			if (kthread_should_stop())
				break;
			spin_lock_irqsave(&(events_group.cpu_hotplug_lock),
									flags);
		}

		set_current_state(TASK_RUNNING);
		notify_change = events_group.cpu_hotplug;
		events_group.cpu_hotplug = false;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);

		if (notify_change)
			sysfs_notify(events_kobj, NULL, "cpu_hotplug");
	}

	return 0;
}

static int init_notify_group(void)
{
	int ret;
	struct kobject *module_kobj;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	notify_kobj = kobject_create_and_add("notify", module_kobj);
	if (!notify_kobj) {
		pr_err("msm_perf: Failed to add notify_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(notify_kobj, &notify_attr_group);
	if (ret) {
		kobject_put(notify_kobj);
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}
	return 0;
}

static int init_events_group(void)
{
	int ret;
	struct kobject *module_kobj;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	events_kobj = kobject_create_and_add("events", module_kobj);
	if (!events_kobj) {
		pr_err("msm_perf: Failed to add events_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}

	spin_lock_init(&(events_group.cpu_hotplug_lock));
	events_notify_thread = kthread_run(events_notify_userspace,
					NULL, "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread))
		return PTR_ERR(events_notify_thread);

	events_group.init_success = true;

	return 0;
}

static void nr_notify_userspace(struct work_struct *work)
{
	sysfs_notify(notify_kobj, NULL, "aggr_top_load");
	sysfs_notify(notify_kobj, NULL, "aggr_big_nr");
	sysfs_notify(notify_kobj, NULL, "top_load_cluster");
	sysfs_notify(notify_kobj, NULL, "curr_cap_cluster");
}

#ifdef CONFIG_SCHED_WALT
static int msm_perf_core_ctl_notify(struct notifier_block *nb,
						unsigned long unused,
						void *data)
{
	static unsigned int tld, nrb, i;
	static unsigned int top_ld[CLUSTER_MAX] = {0}, curr_cp[CLUSTER_MAX] = {0};
	static DECLARE_WORK(sysfs_notify_work, nr_notify_userspace);
	unsigned int new_big_nr, new_top_load;
	unsigned int new_cluster_top[CLUSTER_MAX];
	unsigned int new_cluster_cap[CLUSTER_MAX];
	struct core_ctl_notif_data *d = data;
	bool changed = false;
	int cluster = 0;

	nrb += d->nr_big;
	tld += d->coloc_load_pct;
	for (cluster = 0; cluster < CLUSTER_MAX; cluster++) {
		top_ld[cluster] += d->ta_util_pct[cluster];
		curr_cp[cluster] += d->cur_cap_pct[cluster];
	}
	i++;
	if (i == POLL_INT) {
		new_big_nr = ((nrb % POLL_INT) ? 1 : 0) + nrb / POLL_INT;
		new_top_load = tld / POLL_INT;

		changed = (new_big_nr != aggr_big_nr) ||
			  (new_top_load != aggr_top_load);
		for (cluster = 0; cluster < CLUSTER_MAX; cluster++) {
			new_cluster_top[cluster] = top_ld[cluster] / POLL_INT;
			new_cluster_cap[cluster] = curr_cp[cluster] / POLL_INT;
			changed |= (new_cluster_top[cluster] != top_load[cluster]);
			changed |= (new_cluster_cap[cluster] != curr_cap[cluster]);

			top_load[cluster] = new_cluster_top[cluster];
			curr_cap[cluster] = new_cluster_cap[cluster];
			top_ld[cluster] = 0;
			curr_cp[cluster] = 0;
		}
		aggr_big_nr = new_big_nr;
		aggr_top_load = new_top_load;

		tld = 0;
		nrb = 0;
		i = 0;
		if (changed)
			schedule_work(&sysfs_notify_work);
	}
	return NOTIFY_OK;
}

static struct notifier_block msm_perf_nb = {
	.notifier_call = msm_perf_core_ctl_notify
};

static bool core_ctl_register;
static int set_core_ctl_register(const char *buf, const struct kernel_param *kp)
{
	int ret;
	bool old_val = core_ctl_register;

	ret = param_set_bool(buf, kp);
	if (ret < 0)
		return ret;

	if (core_ctl_register == old_val)
		return 0;

	if (core_ctl_register)
		core_ctl_notifier_register(&msm_perf_nb);
	else
		core_ctl_notifier_unregister(&msm_perf_nb);

	return 0;
}

static const struct kernel_param_ops param_ops_cc_register = {
	.set = set_core_ctl_register,
	.get = param_get_bool,
};
module_param_cb(core_ctl_register, &param_ops_cc_register,
		&core_ctl_register, 0644);
#else
static DECLARE_WORK(msm_perf_sysfs_notify_work, nr_notify_userspace);
static bool msm_perf_poll_enable = true;
static unsigned int msm_perf_poll_ms = 40;
static unsigned int msm_perf_poll_window = 5;
static unsigned int msm_perf_big_util_min = 1;
static bool msm_perf_poll_initialized;
static bool core_ctl_register = true;
static void msm_perf_poll_notify_userspace(struct work_struct *work);
static DECLARE_DELAYED_WORK(msm_perf_poll_work, msm_perf_poll_notify_userspace);

static unsigned int msm_perf_accum_big_nr;
static unsigned int msm_perf_accum_top_load;
static unsigned int msm_perf_accum_top_load_cluster[CLUSTER_MAX];
static unsigned int msm_perf_accum_curr_cap_cluster[CLUSTER_MAX];
static unsigned int msm_perf_accum_samples;
static bool msm_perf_poll_suspended;

static inline unsigned long msm_perf_poll_delay_jiffies(void)
{
	return msecs_to_jiffies(max_t(unsigned int, 10, msm_perf_poll_ms));
}

static inline void msm_perf_rearm_poll_work(unsigned long delay_jiffies)
{
	cancel_delayed_work_sync(&msm_perf_poll_work);
	if (msm_perf_poll_enable && !msm_perf_poll_suspended)
		schedule_delayed_work(&msm_perf_poll_work, delay_jiffies);
}

static void msm_perf_reset_compat_accumulators(void)
{
	msm_perf_accum_big_nr = 0;
	msm_perf_accum_top_load = 0;
	memset(msm_perf_accum_top_load_cluster, 0, sizeof(msm_perf_accum_top_load_cluster));
	memset(msm_perf_accum_curr_cap_cluster, 0, sizeof(msm_perf_accum_curr_cap_cluster));
	msm_perf_accum_samples = 0;
}

static int set_compat_poll_ms(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;

	msm_perf_poll_ms = clamp_t(unsigned int, msm_perf_poll_ms, 10, 1000);
	if (msm_perf_poll_initialized)
		msm_perf_rearm_poll_work(msm_perf_poll_delay_jiffies());

	return 0;
}

static int set_compat_poll_enable(const char *val, const struct kernel_param *kp)
{
	bool old = msm_perf_poll_enable;
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;

	if (!msm_perf_poll_initialized || old == msm_perf_poll_enable)
		return 0;

	core_ctl_register = msm_perf_poll_enable;
	msm_perf_reset_compat_accumulators();

	msm_perf_rearm_poll_work(msm_perf_poll_delay_jiffies());

	return 0;
}

static int set_compat_poll_window(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;

	msm_perf_poll_window = clamp_t(unsigned int, msm_perf_poll_window, 1, 50);
	msm_perf_reset_compat_accumulators();

	return 0;
}

static const struct kernel_param_ops param_ops_compat_poll_enable = {
	.set = set_compat_poll_enable,
	.get = param_get_bool,
};
module_param_cb(compat_poll_enable, &param_ops_compat_poll_enable,
		&msm_perf_poll_enable, 0644);

static const struct kernel_param_ops param_ops_compat_poll_ms = {
	.set = set_compat_poll_ms,
	.get = param_get_uint,
};
module_param_cb(compat_poll_ms, &param_ops_compat_poll_ms,
		&msm_perf_poll_ms, 0644);

static const struct kernel_param_ops param_ops_compat_poll_window = {
	.set = set_compat_poll_window,
	.get = param_get_uint,
};
module_param_cb(compat_poll_window, &param_ops_compat_poll_window,
		&msm_perf_poll_window, 0644);

static int set_compat_big_util_min(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;

	msm_perf_big_util_min = clamp_t(unsigned int, msm_perf_big_util_min, 1, 100);
	return 0;
}

static const struct kernel_param_ops param_ops_compat_big_util_min = {
	.set = set_compat_big_util_min,
	.get = param_get_uint,
};
module_param_cb(compat_big_util_min, &param_ops_compat_big_util_min,
		&msm_perf_big_util_min, 0644);

static int set_core_ctl_register_compat(const char *val,
					const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;

	msm_perf_poll_enable = core_ctl_register;
	if (!msm_perf_poll_initialized)
		return 0;

	msm_perf_reset_compat_accumulators();
	msm_perf_rearm_poll_work(0);

	return 0;
}

static const struct kernel_param_ops param_ops_cc_register = {
	.set = set_core_ctl_register_compat,
	.get = param_get_bool,
};
module_param_cb(core_ctl_register, &param_ops_cc_register,
		&core_ctl_register, 0644);

static bool msm_perf_update_load_pct(void)
{
	unsigned int cluster_load_sum[CLUSTER_MAX] = {0}, pub_top_load[CLUSTER_MAX];
	unsigned int cluster_cap_sum[CLUSTER_MAX] = {0}, pub_curr_cap[CLUSTER_MAX];
	unsigned int cluster_cpu_cnt[CLUSTER_MAX] = {0}, pub_big_nr, pub_top;
	unsigned int max_cluster_busy = 0, total_pct = 0, total_cpus = 0;
	bool changed = false;
	int cpu;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		unsigned long cap = perf_cpu_capacity[cpu];
		unsigned long util, thermal, cap_pct;
		unsigned int util_pct;
		u8 cluster = per_cpu(perf_cluster_id, cpu);

		if (!cap)
			continue;

		util = min_t(unsigned long, sched_cpu_util(cpu, cap), cap);
		util_pct = mult_frac(util, 100, cap);

		thermal = min_t(unsigned long, arch_scale_thermal_pressure(cpu), cap);
		cap_pct = mult_frac(cap - thermal, 100, cap);

		cluster_load_sum[cluster] += util_pct;
		cluster_cap_sum[cluster] += cap_pct;
		cluster_cpu_cnt[cluster]++;
		total_pct += util_pct;
		total_cpus++;

			if (cluster == MAX && util_pct >= msm_perf_big_util_min)
				max_cluster_busy++;
	}
	cpus_read_unlock();

	for (cpu = 0; cpu < CLUSTER_MAX; cpu++) {
		if (cluster_cpu_cnt[cpu]) {
			pub_top_load[cpu] = cluster_load_sum[cpu] / cluster_cpu_cnt[cpu];
			pub_curr_cap[cpu] = cluster_cap_sum[cpu] / cluster_cpu_cnt[cpu];
		} else {
			pub_top_load[cpu] = 0;
			pub_curr_cap[cpu] = 0;
		}
	}

	pub_big_nr = max_cluster_busy;
	pub_top = total_cpus ? (total_pct / total_cpus) : 0;

	msm_perf_accum_big_nr += pub_big_nr;
	msm_perf_accum_top_load += pub_top;
	for (cpu = 0; cpu < CLUSTER_MAX; cpu++) {
		msm_perf_accum_top_load_cluster[cpu] += pub_top_load[cpu];
		msm_perf_accum_curr_cap_cluster[cpu] += pub_curr_cap[cpu];
	}
	msm_perf_accum_samples++;

	if (msm_perf_accum_samples < max_t(unsigned int, 1, msm_perf_poll_window))
		return false;

	pub_big_nr = DIV_ROUND_CLOSEST(msm_perf_accum_big_nr, msm_perf_accum_samples);
	pub_top = DIV_ROUND_CLOSEST(msm_perf_accum_top_load, msm_perf_accum_samples);
	changed |= (READ_ONCE(aggr_big_nr) != pub_big_nr);
	changed |= (READ_ONCE(aggr_top_load) != pub_top);
	WRITE_ONCE(aggr_big_nr, pub_big_nr);
	WRITE_ONCE(aggr_top_load, pub_top);

	for (cpu = 0; cpu < CLUSTER_MAX; cpu++) {
		pub_top_load[cpu] = DIV_ROUND_CLOSEST(msm_perf_accum_top_load_cluster[cpu],
						      msm_perf_accum_samples);
		pub_curr_cap[cpu] = DIV_ROUND_CLOSEST(msm_perf_accum_curr_cap_cluster[cpu],
						      msm_perf_accum_samples);
		changed |= (READ_ONCE(top_load[cpu]) != pub_top_load[cpu]);
		changed |= (READ_ONCE(curr_cap[cpu]) != pub_curr_cap[cpu]);
		WRITE_ONCE(top_load[cpu], pub_top_load[cpu]);
		WRITE_ONCE(curr_cap[cpu], pub_curr_cap[cpu]);
		msm_perf_accum_top_load_cluster[cpu] = 0;
		msm_perf_accum_curr_cap_cluster[cpu] = 0;
	}

	msm_perf_accum_big_nr = 0;
	msm_perf_accum_top_load = 0;
	msm_perf_accum_samples = 0;

	return changed;
}

static void msm_perf_poll_notify_userspace(struct work_struct *work)
{
	if (msm_perf_poll_suspended)
		return;

	if (msm_perf_update_load_pct())
		schedule_work(&msm_perf_sysfs_notify_work);
	if (msm_perf_poll_enable)
		schedule_delayed_work(to_delayed_work(work), msm_perf_poll_delay_jiffies());
}

static int msm_perf_pm_notifier(struct notifier_block *nb,
				unsigned long action, void *unused)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		msm_perf_poll_suspended = true;
		cancel_delayed_work_sync(&msm_perf_poll_work);
		break;
	case PM_POST_SUSPEND:
		msm_perf_poll_suspended = false;
		msm_perf_reset_compat_accumulators();
		if (msm_perf_poll_enable)
			schedule_delayed_work(&msm_perf_poll_work, 0);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block msm_perf_pm_nb = {
	.notifier_call = msm_perf_pm_notifier,
};

#endif

void  msm_perf_events_update(enum evt_update_t update_typ,
			enum gfx_evt_t evt_typ, pid_t pid,
			uint32_t ctx_id, uint32_t timestamp, bool end_of_frame)
{
	unsigned long flags;
	int idx = 0;

	if (update_typ != MSM_PERF_GFX)
		return;

	if (pid != atomic_read(&game_status_pid) || (timestamp == 0)
		|| !(end_of_frame))
		return;

	spin_lock_irqsave(&gfx_circ_buff_lock, flags);
	idx = curr_pos.head;
	curr_pos.head = ((curr_pos.head + 1) % QUEUE_POOL_SIZE);
	spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);
	gpu_circ_buff[idx].pid = pid;
	gpu_circ_buff[idx].ctx_id = ctx_id;
	gpu_circ_buff[idx].timestamp = timestamp;
	gpu_circ_buff[idx].evt_typ = evt_typ;
	gpu_circ_buff[idx].arrive_ts = ktime_get();

	if (evt_typ == MSM_PERF_QUEUE || evt_typ == MSM_PERF_RETIRED)
		complete(&gfx_evt_arrival);
}


static int set_game_start_pid(const char *buf, const struct kernel_param *kp)
{
	long usr_val = 0;
	int ret;

	ret = kstrtol(buf, 0, &usr_val);
	if (ret) {
		pr_err("msm_perf: kstrtol failed, ret=%d\n", ret);
		return ret;
	}
	ret = strlen(buf);
	atomic_set(&game_status_pid, usr_val);
	return ret;
}

static int get_game_start_pid(char *buf, const struct kernel_param *kp)
{
	long usr_val  = atomic_read(&game_status_pid);

	return scnprintf(buf, PAGE_SIZE, "%ld\n", usr_val);
}

static const struct kernel_param_ops param_ops_game_start_pid = {
	.set = set_game_start_pid,
	.get = get_game_start_pid,
};
module_param_cb(evnt_gplaf_pid, &param_ops_game_start_pid, NULL, 0644);
#endif /* CONFIG_MSM_PERFORMANCE_QGKI */

/*******************************GFX Call************************************/
#ifdef CONFIG_QTI_PLH
static struct scmi_handle *plh_handle;
void rimps_plh_init(struct scmi_handle *handle)
{
	if (handle)
		plh_handle = handle;
}
EXPORT_SYMBOL(rimps_plh_init);

static int splh_notif, splh_init_done, plh_log_level;

#define PLH_MIN_LOG_LEVEL			0
#define PLH_MAX_LOG_LEVEL			0xF
#define SPLH_FPS_MAX_CNT			8
#define SPLH_IPC_FREQ_VTBL_MAX_CNT		5 /* ipc freq pair */
#define SPLH_INIT_IPC_FREQ_TBL_PARAMS	\
			(2 + SPLH_FPS_MAX_CNT * (1 + (2 * SPLH_IPC_FREQ_VTBL_MAX_CNT)))

static int set_plh_log_level(const char *buf, const struct kernel_param *kp)
{
	int ret, log_val_backup;
	struct scmi_plh_vendor_ops *ops;

	if (!plh_handle || !plh_handle->plh_ops) {
		pr_err("msm_perf: plh scmi handle or vendor ops null\n");
		return -EINVAL;
	}

	ops = plh_handle->plh_ops;

	log_val_backup = plh_log_level;

	ret = param_set_int(buf, kp); /* Updates plh_log_level */
	if (ret < 0) {
		pr_err("msm_perf: getting new plh_log_level failed, ret=%d\n", ret);
		return ret;
	}

	plh_log_level = clamp(plh_log_level, PLH_MIN_LOG_LEVEL, PLH_MAX_LOG_LEVEL);
	ret = ops->set_plh_log_level(plh_handle, plh_log_level);
	if (ret < 0) {
		plh_log_level = log_val_backup;
		pr_err("msm_perf: setting new plh_log_level failed, ret=%d\n", ret);
		return ret;
	}
	return 0;
}

static const struct kernel_param_ops param_ops_plh_log_level = {
	.set = set_plh_log_level,
	.get = param_get_int,
};
module_param_cb(plh_log_level, &param_ops_plh_log_level, &plh_log_level, 0644);

static int init_splh_notif(const char *buf)
{
	int i, j, ret;
	u16 tmp[SPLH_INIT_IPC_FREQ_TBL_PARAMS] = {0};
	u16 *ptmp = tmp, ntokens, nfps, n_ipc_freq_pair, tmp_valid_len = 0;
	const char *cp, *cp1;
	struct scmi_plh_vendor_ops *ops;

	/* buf contains the init info from user */
	if (buf == NULL || !plh_handle || !plh_handle->plh_ops)
		return -EINVAL;

	cp = buf;
	ntokens = 0;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	/* format of cmd nfps, n_ipc_freq_pair, <fps0, <ipc0, freq0>,...>,... */
	cp = buf;
	if (sscanf(cp, INIT ":%hu", &nfps)) {
		if ((nfps != ntokens-1) || (nfps == 0) || (nfps > SPLH_FPS_MAX_CNT))
			return -EINVAL;

		cp = strnchr(cp, strlen(cp), ':');	/* skip INIT */
		cp++;
		cp = strnchr(cp, strlen(cp), ':');	/* skip nfps */
		if (!cp)
			return -EINVAL;

		*ptmp++ = nfps;		/* nfps is first cmd param */
		tmp_valid_len++;
		cp1 = cp;
		ntokens = 0;
		/* get count of nfps * n_ipc_freq_pair * <ipc freq pair values> */
		while ((cp1 = strpbrk(cp1 + 1, ",")))
			ntokens++;

		if (ntokens % (2 * nfps)) /* ipc freq pair values should be multiple of nfps */
			return -EINVAL;

		n_ipc_freq_pair = ntokens / (2 * nfps); /* ipc_freq pair values for each FPS */
		if ((n_ipc_freq_pair == 0) || (n_ipc_freq_pair > SPLH_IPC_FREQ_VTBL_MAX_CNT))
			return -EINVAL;

		*ptmp++ = n_ipc_freq_pair; /* n_ipc_freq_pair is second cmd param */
		tmp_valid_len++;
		cp1 = cp;
		for (i = 0; i < nfps; i++) {
			if (sscanf(cp1, ":%hu", ptmp) != 1)
				return -EINVAL;

			ptmp++;		/* increment after storing FPS val */
			tmp_valid_len++;
			cp1 = strnchr(cp1, strlen(cp1), ','); /* move to ,ipc */
			if (!cp1)
				return -EINVAL;

			for (j = 0; j < 2 * n_ipc_freq_pair; j++) {
				if (sscanf(cp1, ",%hu", ptmp) != 1)
					return -EINVAL;

				ptmp++;	/* increment after storing ipc or freq */
				tmp_valid_len++;
				cp1++;
				if (j != (2 * n_ipc_freq_pair - 1)) {
					cp1 = strnchr(cp1, strlen(cp1), ','); /* move to next */
					if (!cp1)
						return -EINVAL;

				}
			}

			if (i != (nfps - 1)) {
				cp1 = strnchr(cp1, strlen(cp1), ':'); /* move to next FPS val */
				if (!cp1)
					return -EINVAL;

			}

		}
	} else {
		return -EINVAL;
	}

	ops = plh_handle->plh_ops;
	ret = ops->init_splh_ipc_freq_tbl(plh_handle, tmp, tmp_valid_len);
	if (ret < 0)
		return -EINVAL;

	pr_info("msm_perf: nfps=%hu n_ipc_freq_pair=%hu last_freq_val=%hu len=%hu\n",
		nfps, n_ipc_freq_pair, *--ptmp, tmp_valid_len);
	splh_init_done = 1;
	return 0;
}

static void activate_splh_notif(void)
{
	int ret;
	struct scmi_plh_vendor_ops *ops;
	/* received event notification here */
	if (!plh_handle || !plh_handle->plh_ops) {
		pr_err("msm_perf: splh not supported\n");
		return;
	}
	ops = plh_handle->plh_ops;

	if (splh_notif)
		ret = ops->start_splh(plh_handle, splh_notif); /* splh_notif is fps */
	else
		ret = ops->stop_splh(plh_handle);

	if (ret < 0) {
		pr_err("msm_perf: splh start or stop failed, ret=%d\n", ret);
		return;
	}
}

static int set_splh_notif(const char *buf, const struct kernel_param *kp)
{
	int ret;

	if (strnstr(buf, INIT, sizeof(INIT)) != NULL) {
		splh_init_done = 0;
		ret = init_splh_notif(buf);
		if (ret < 0)
			pr_err("msm_perf: splh ipc freq tbl init failed, ret=%d\n", ret);

		return ret;
	}

	if (!splh_init_done) {
		pr_err("msm_perf: splh ipc freq tbl not initialized\n");
		return -EINVAL;
	}

	ret = param_set_int(buf, kp);
	if (ret < 0)
		return ret;

	activate_splh_notif();

	return 0;
}

static const struct kernel_param_ops param_ops_splh_notification = {
	.set = set_splh_notif,
	.get = param_get_int,
};
module_param_cb(splh_notif, &param_ops_splh_notification, &splh_notif, 0644);
#endif /* CONFIG_QTI_PLH */

static int __init msm_performance_init(void)
{
#ifdef CONFIG_MSM_PERFORMANCE_QGKI
	unsigned int cpu;
	int ret;
#endif
	if (!alloc_cpumask_var(&limit_mask_min, GFP_KERNEL))
		return -ENOMEM;

	if (!alloc_cpumask_var(&limit_mask_max, GFP_KERNEL)) {
		free_cpumask_var(limit_mask_min);
		return -ENOMEM;
	}
#ifdef CONFIG_MSM_PERFORMANCE_QGKI
	get_online_cpus();
	for_each_possible_cpu(cpu) {
		if (!cpumask_test_cpu(cpu, cpu_online_mask))
			per_cpu(cpu_is_hp, cpu) = true;
	}

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
		"msm_performance_cpu_hotplug",
		hotplug_notify_up,
		hotplug_notify_down);

	put_online_cpus();

	init_events_group();
	init_notify_group();
	init_pmu_counter();

	idle_notifier_register(&msm_perf_event_idle_nb);
#ifndef CONFIG_SCHED_WALT
	msm_perf_poll_ms = clamp_t(unsigned int, msm_perf_poll_ms, 10, 1000);
	msm_perf_poll_window = clamp_t(unsigned int, msm_perf_poll_window, 1, 50);
	msm_perf_big_util_min = clamp_t(unsigned int, msm_perf_big_util_min, 1, 100);
	msm_perf_poll_initialized = true;
	register_pm_notifier(&msm_perf_pm_nb);
	if (msm_perf_poll_enable)
		schedule_delayed_work(&msm_perf_poll_work,
				msm_perf_poll_delay_jiffies());
#endif
#endif
	return 0;
}
MODULE_LICENSE("GPL v2");
late_initcall(msm_performance_init);
