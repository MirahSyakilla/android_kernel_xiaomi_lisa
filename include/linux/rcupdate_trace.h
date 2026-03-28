/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Trace-oriented RCU reader and callback helpers.
 *
 * This tree carries newer BPF users that expect the trace-RCU API surface.
 * Provide compatibility mappings over existing RCU/task-RCU primitives.
 */

#ifndef __LINUX_RCUPDATE_TRACE_H
#define __LINUX_RCUPDATE_TRACE_H

#include <linux/rcupdate.h>

static inline int rcu_read_lock_trace_held(void)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	return rcu_read_lock_held();
#else
	return 1;
#endif
}

static inline void rcu_read_lock_trace(void)
{
	rcu_read_lock();
}

static inline void rcu_read_unlock_trace(void)
{
	rcu_read_unlock();
}

static inline void call_rcu_tasks_trace(struct rcu_head *rhp,
					rcu_callback_t func)
{
#ifdef CONFIG_TASKS_RCU
	call_rcu_tasks(rhp, func);
#else
	call_rcu(rhp, func);
#endif
}

static inline void synchronize_rcu_tasks_trace(void)
{
#ifdef CONFIG_TASKS_RCU
	synchronize_rcu_tasks();
#else
	synchronize_rcu();
#endif
}

static inline void rcu_barrier_tasks_trace(void)
{
#ifdef CONFIG_TASKS_RCU
	rcu_barrier_tasks();
#else
	synchronize_rcu();
#endif
}

#endif /* __LINUX_RCUPDATE_TRACE_H */
