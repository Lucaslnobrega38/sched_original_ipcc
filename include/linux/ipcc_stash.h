/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deferred COW for IPC-class shadow clones. See context.md for the design
 * (why the target pays instead of the shadow, why the payload lives on a
 * side list instead of the shadow's own ptes, and the VM_SHARED correctness
 * fix this depends on).
 */
#ifndef _LINUX_IPCC_STASH_H
#define _LINUX_IPCC_STASH_H

#include <linux/jump_label.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/percpu.h>
#include <linux/types.h>

#ifdef CONFIG_IPC_CLASSES_SHADOW_DEFER_COW

enum ipcc_stash_counter {
	/* Parent write-faults that took the deferred path. */
	IPCC_STASH_CREATED,
	/* Shadow faulted on a stash and materialised it into a real page. */
	IPCC_STASH_CONSUMED,
	/* Shadow died without ever touching the stash - the whole point. */
	IPCC_STASH_DISCARDED,

	/* Fast path declined; each falls back to unmodified wp_page_copy(). */
	IPCC_STASH_DECLINED_PINNED,	/* folio may be DMA-pinned */
	IPCC_STASH_DECLINED_LARGE,	/* THP/large folio, not handled in v1 */
	IPCC_STASH_DECLINED_VMALOCK,	/* under per-VMA lock, needs mmap_lock */
	IPCC_STASH_DECLINED_MMGONE,	/* shadow mm already tearing down */
	IPCC_STASH_DECLINED_NOMEM,	/* stash allocation failed */
	IPCC_STASH_DECLINED_OTHER,	/* mapcount not explained by shadows etc */

	/*
	 * Parent paid a full wp_page_copy() on a page shared with a live
	 * shadow. With the static key off this is the baseline the mechanism
	 * is trying to remove; with it on it should approach zero.
	 */
	IPCC_STASH_PARENT_WP_COPY,

	/* Lifetime accounting for the mm<->mm linkage itself. */
	IPCC_STASH_SHADOW_LINKED,
	IPCC_STASH_SHADOW_UNLINKED,

	IPCC_STASH_NR_COUNTERS
};

struct ipcc_stash_pcpu {
	unsigned long c[IPCC_STASH_NR_COUNTERS];
};

DECLARE_PER_CPU(struct ipcc_stash_pcpu, ipcc_stash_pcpu);

/* Per-cpu, not a shared atomic - summed on read in debugfs. */
static inline void ipcc_stash_count(enum ipcc_stash_counter which)
{
	this_cpu_inc(ipcc_stash_pcpu.c[which]);
}

DECLARE_STATIC_KEY_FALSE(ipcc_stash_enabled);

/* Runtime gate, default off - /sys/kernel/debug/ipcc_stash/enabled. */
static inline bool ipcc_stash_active(void)
{
	return static_branch_unlikely(&ipcc_stash_enabled);
}

/* One stashed page, handed to the shadow's mm under the target's
 * ipcc_shadows_lock, freed by whoever consumes or drains it.
 */
struct ipcc_stash_entry {
	struct list_head node;
	unsigned long addr;		/* page-aligned, in the shadow's mm */
	struct folio *folio;		/* contents as of the fork */
};

void ipcc_stash_mm_init(struct mm_struct *mm);
void ipcc_stash_link_shadow(struct mm_struct *shadow_mm,
			    struct mm_struct *target_mm);
void ipcc_stash_unlink_shadow(struct mm_struct *mm);

void ipcc_stash_arm_vma(struct vm_area_struct *vma);
bool ipcc_stash_offer(struct mm_struct *target_mm, unsigned long addr,
		      struct folio *folio);
struct folio *ipcc_stash_take(struct mm_struct *shadow_mm, unsigned long addr);

/* Cheap "is this mm a shadow target" test for the write fault path.
 * Deliberately unlocked - see context.md; every caller re-checks under
 * ipcc_shadows_lock before acting.
 */
static inline bool ipcc_mm_has_shadows(struct mm_struct *mm)
{
	return !list_empty(&mm->ipcc_shadows);
}

/* Counts a COW break paid on a shadow's behalf, for measurement only. */
static inline void ipcc_stash_note_parent_wp_copy(struct mm_struct *mm)
{
	if (ipcc_mm_has_shadows(mm))
		ipcc_stash_count(IPCC_STASH_PARENT_WP_COPY);
}

#else /* !CONFIG_IPC_CLASSES_SHADOW_DEFER_COW */

static inline void ipcc_stash_mm_init(struct mm_struct *mm) { }
static inline void ipcc_stash_link_shadow(struct mm_struct *shadow_mm,
					  struct mm_struct *target_mm) { }
static inline void ipcc_stash_unlink_shadow(struct mm_struct *mm) { }
static inline bool ipcc_stash_active(void) { return false; }
static inline bool ipcc_mm_has_shadows(struct mm_struct *mm) { return false; }
static inline void ipcc_stash_note_parent_wp_copy(struct mm_struct *mm) { }
static inline void ipcc_stash_arm_vma(struct vm_area_struct *vma) { }
static inline bool ipcc_stash_offer(struct mm_struct *target_mm,
				    unsigned long addr, struct folio *folio)
{
	return false;
}
static inline struct folio *ipcc_stash_take(struct mm_struct *shadow_mm,
					    unsigned long addr)
{
	return NULL;
}

#endif /* CONFIG_IPC_CLASSES_SHADOW_DEFER_COW */

#endif /* _LINUX_IPCC_STASH_H */
