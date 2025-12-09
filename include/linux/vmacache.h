/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_VMACACHE_H
#define __LINUX_VMACACHE_H

#include <linux/mm.h>

static inline void vmacache_flush(struct task_struct *tsk) { }
static inline void vmacache_invalidate(struct mm_struct *mm) { }
static inline void vmacache_update(unsigned long addr, struct vm_area_struct *newvma) { }

static inline struct vm_area_struct *vmacache_find(struct mm_struct *mm, unsigned long addr)
{
	return NULL;
}

#ifndef CONFIG_MMU
static inline struct vm_area_struct *vmacache_find_exact(struct mm_struct *mm,
						  unsigned long start,
						  unsigned long end)
{
	return NULL;
}
#endif

#endif /* __LINUX_VMACACHE_H */