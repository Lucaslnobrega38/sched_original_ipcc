/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deferred COW for IPC-class shadow clones.
 *
 * A shadow clone (see kernel/fork.c:shadow_kernel_clone()) is an ordinary COW
 * fork of a real task, so for as long as it lives every write the real task
 * makes to a still-shared page costs that task a full COW break in
 * wp_page_copy(): allocate a folio, copy 4KB, wire it into rmap and the LRU.
 * Shadows normally die in under a millisecond having touched almost nothing,
 * so that is usually paid for a snapshot that is never read.
 *
 * This inverts it. On such a write the real task instead preserves the old
 * contents into a bare "stash" folio - refcount 1, deliberately never added to
 * rmap or the LRU - and keeps writing to the original page in place. The
 * shadow's pte for that address is retargeted to a special non-present entry
 * carrying the stash's pfn. Only if the shadow actually faults there does it
 * pay to turn the stash into a real anonymous page. A shadow that dies first
 * has its stash freed straight out of the zap path, having never cost anything
 * more than the copy.
 *
 * Per-shadow state needs no side table: the stash pfn lives in the shadow's
 * own pte, and each shadow has its own page tables. Two shadows of the same
 * target forked at different times legitimately hold *different* snapshots of
 * the same address (the target may have written in between), which falls out
 * of this for free.
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

/*
 * Per-cpu rather than a shared atomic: these are incremented from the write
 * fault path, and a single global cacheline pinged by every faulting cpu would
 * cost more than the mechanism saves. Summed on read in debugfs.
 */
static inline void ipcc_stash_count(enum ipcc_stash_counter which)
{
	this_cpu_inc(ipcc_stash_pcpu.c[which]);
}

DECLARE_STATIC_KEY_FALSE(ipcc_stash_enabled);

/*
 * Runtime gate, default off, flipped via /sys/kernel/debug/ipcc_stash/enabled.
 * Lets the deferred path be A/B'd against stock COW within one boot, and keeps
 * it inert by default even in kernels built with the Kconfig on.
 */
static inline bool ipcc_stash_active(void)
{
	return static_branch_unlikely(&ipcc_stash_enabled);
}

void ipcc_stash_mm_init(struct mm_struct *mm);
void ipcc_stash_link_shadow(struct mm_struct *shadow_mm,
			    struct mm_struct *target_mm);
void ipcc_stash_unlink_shadow(struct mm_struct *mm);

/*
 * Cheap "is this mm a shadow target at all" test for the write fault path.
 *
 * Deliberately unlocked: list_empty() here races with a concurrent
 * shadow_copy_process() linking a new shadow, so a false "empty" is possible
 * and harmless (the fault simply takes the stock COW path it would have taken
 * anyway). A false "non-empty" is likewise harmless because every caller
 * re-checks under ipcc_shadows_lock before acting. What it must never do is
 * make the common case - an mm that has never had a shadow - pay for a lock.
 */
static inline bool ipcc_mm_has_shadows(struct mm_struct *mm)
{
	return !list_empty(&mm->ipcc_shadows);
}

/**
 * ipcc_stash_note_parent_wp_copy - count a COW break paid on a shadow's behalf
 * @mm: mm of the task taking the write fault
 *
 * Called from do_wp_page() on the path to wp_page_copy(), purely to measure.
 * This is exactly the event the deferred mechanism exists to remove: a real
 * task about to pay allocate + 4KB copy + rmap + LRU while a shadow clone of
 * it is alive and still sharing pages - usually for a snapshot that shadow
 * dies without ever reading.
 *
 * Deliberately does not try to prove the extra mapcount is *caused by* a
 * shadow, so this is an upper bound on what the mechanism could recover.
 * Knowing that bound is what says whether the rest is worth building at all.
 */
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

#endif /* CONFIG_IPC_CLASSES_SHADOW_DEFER_COW */

#endif /* _LINUX_IPCC_STASH_H */
