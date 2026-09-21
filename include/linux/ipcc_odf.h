/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IPCC_ODF_H
#define _LINUX_IPCC_ODF_H

#include <linux/jump_label.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/types.h>

struct vm_fault;

#ifdef CONFIG_IPC_CLASSES_SHADOW_ODF

enum ipcc_odf_counter {
	IPCC_ODF_LOGGED,
	IPCC_ODF_LOG_FAILED,
	IPCC_ODF_LOG_DUP,
	IPCC_ODF_DISCARDED,
	IPCC_ODF_FROM_LOG,
	IPCC_ODF_FROM_TARGET,
	IPCC_ODF_MISSED,
	IPCC_ODF_NOMEM,
	IPCC_ODF_WPROTECTED,
	IPCC_ODF_SHADOW_LINKED,
	IPCC_ODF_SHADOW_UNLINKED,
	IPCC_ODF_NR_COUNTERS
};

struct ipcc_odf_pcpu {
	unsigned long c[IPCC_ODF_NR_COUNTERS];
};

DECLARE_PER_CPU(struct ipcc_odf_pcpu, ipcc_odf_pcpu);

static inline void ipcc_odf_count(enum ipcc_odf_counter which)
{
	this_cpu_inc(ipcc_odf_pcpu.c[which]);
}

DECLARE_STATIC_KEY_FALSE(ipcc_odf_enabled);

static inline bool ipcc_odf_active(void)
{
	return static_branch_unlikely(&ipcc_odf_enabled);
}

static inline bool ipcc_mm_has_shadows(struct mm_struct *mm)
{
	return !list_empty(&mm->ipcc_shadows);
}

static inline bool ipcc_mm_is_shadow(struct mm_struct *mm)
{
	return mm->ipcc_shadow_of;
}

void ipcc_odf_mm_init(struct mm_struct *mm);
void ipcc_odf_link_shadow(struct mm_struct *shadow_mm,
			  struct mm_struct *target_mm);
void ipcc_odf_unlink_shadow(struct mm_struct *mm);

void ipcc_odf_wrprotect_target(struct mm_struct *mm);
void ipcc_odf_log(struct mm_struct *target_mm, unsigned long addr,
		  struct folio *folio);
bool ipcc_odf_populate(struct vm_fault *vmf, vm_fault_t *ret);

static inline bool ipcc_odf_forking(void)
{
	return current->ipcc_odf_fork;
}

static inline bool ipcc_odf_wants_log(struct mm_struct *mm)
{
	return ipcc_odf_active() && ipcc_mm_has_shadows(mm);
}

static inline bool ipcc_odf_wants_populate(struct vm_area_struct *vma)
{
	return ipcc_odf_active() && ipcc_mm_is_shadow(vma->vm_mm);
}

#else /* !CONFIG_IPC_CLASSES_SHADOW_ODF */

static inline void ipcc_odf_mm_init(struct mm_struct *mm) { }
static inline void ipcc_odf_link_shadow(struct mm_struct *shadow_mm,
					struct mm_struct *target_mm) { }
static inline void ipcc_odf_unlink_shadow(struct mm_struct *mm) { }
static inline bool ipcc_odf_active(void) { return false; }
static inline bool ipcc_mm_has_shadows(struct mm_struct *mm) { return false; }
static inline bool ipcc_mm_is_shadow(struct mm_struct *mm) { return false; }
static inline void ipcc_odf_wrprotect_target(struct mm_struct *mm) { }
static inline void ipcc_odf_log(struct mm_struct *target_mm,
				unsigned long addr, struct folio *folio) { }
static inline bool ipcc_odf_forking(void) { return false; }
static inline bool ipcc_odf_wants_log(struct mm_struct *mm) { return false; }
static inline bool ipcc_odf_wants_populate(struct vm_area_struct *vma)
{
	return false;
}
static inline bool ipcc_odf_populate(struct vm_fault *vmf, vm_fault_t *ret)
{
	return false;
}

#endif /* CONFIG_IPC_CLASSES_SHADOW_ODF */

#endif /* _LINUX_IPCC_ODF_H */
