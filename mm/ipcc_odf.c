// SPDX-License-Identifier: GPL-2.0-only
/*
 * On-demand fork at PTE granularity for IPC-class shadow clones.
 */

#define pr_fmt(fmt) "ipcc-odf: " fmt

#include <linux/debugfs.h>
#include <linux/ipcc_odf.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/pagewalk.h>
#include <linux/rmap.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/xarray.h>

#include "internal.h"

DEFINE_PER_CPU(struct ipcc_odf_pcpu, ipcc_odf_pcpu);

DEFINE_STATIC_KEY_FALSE(ipcc_odf_enabled);

void ipcc_odf_mm_init(struct mm_struct *mm)
{
	INIT_LIST_HEAD(&mm->ipcc_shadows);
	spin_lock_init(&mm->ipcc_shadows_lock);
	INIT_LIST_HEAD(&mm->ipcc_shadow_node);
	mm->ipcc_shadow_of = NULL;
	xa_init(&mm->ipcc_odf);
}

void ipcc_odf_link_shadow(struct mm_struct *shadow_mm,
			  struct mm_struct *target_mm)
{
	if (WARN_ON_ONCE(shadow_mm == target_mm))
		return;

	if (WARN_ON_ONCE(target_mm->ipcc_shadow_of))
		return;

	mmgrab(target_mm);
	shadow_mm->ipcc_shadow_of = target_mm; // link

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_add(&shadow_mm->ipcc_shadow_node, &target_mm->ipcc_shadows);
	spin_unlock(&target_mm->ipcc_shadows_lock);

	ipcc_odf_count(IPCC_ODF_SHADOW_LINKED); // debug
}

void ipcc_odf_unlink_shadow(struct mm_struct *mm)
{
	// mm é o shadow 
	struct mm_struct *target_mm = mm->ipcc_shadow_of;
	unsigned long index;
	struct folio *folio;

	// warning: shadow sem target linkado
	if (!target_mm)
		return;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_del_init(&mm->ipcc_shadow_node); // deletamos o shadow node da lista de seu mm
	spin_unlock(&target_mm->ipcc_shadows_lock);

	// destruir o stash antigo 
	xa_for_each(&mm->ipcc_odf, index, folio) {
		xa_erase(&mm->ipcc_odf, index);
		folio_put(folio);
		ipcc_odf_count(IPCC_ODF_DISCARDED);
	}
	xa_destroy(&mm->ipcc_odf);

	mm->ipcc_shadow_of = NULL;
	mmdrop(target_mm);

	ipcc_odf_count(IPCC_ODF_SHADOW_UNLINKED);
}

struct ipcc_odf_wp_walk {
	struct mmu_notifier_range range;
	unsigned long count;
};

static int ipcc_odf_wp_pte(pte_t *pte, unsigned long addr,
			   unsigned long next, struct mm_walk *walk)
{
	struct ipcc_odf_wp_walk *ww = walk->private;
	struct vm_area_struct *vma = walk->vma;
	pte_t ptent = ptep_get(pte);

	if (!pte_present(ptent) || !pte_write(ptent))
		return 0;

	ptep_set_wrprotect(walk->mm, addr, pte);
	ww->count++;

	return 0;
}

static int ipcc_odf_wp_pre_vma(unsigned long start, unsigned long end,
			       struct mm_walk *walk)
{
	struct ipcc_odf_wp_walk *ww = walk->private;

	mmu_notifier_range_init(&ww->range, MMU_NOTIFY_PROTECTION_PAGE, 0,
				walk->mm, start, end);
	mmu_notifier_invalidate_range_start(&ww->range);

	return 0;
}

static void ipcc_odf_wp_post_vma(struct mm_walk *walk)
{
	struct ipcc_odf_wp_walk *ww = walk->private;

	flush_tlb_range(walk->vma, ww->range.start, ww->range.end);
	mmu_notifier_invalidate_range_end(&ww->range);
}

static const struct mm_walk_ops ipcc_odf_wp_ops = {
	.pte_entry	= ipcc_odf_wp_pte,
	.pre_vma	= ipcc_odf_wp_pre_vma,
	.post_vma	= ipcc_odf_wp_post_vma,
	.walk_lock	= PGWALK_RDLOCK,
};

/*
 * Freeze the target's private pages: the next write to each one faults, which
 * is where ipcc_odf_log() gets its chance to keep the fork-time contents.
 * Shared vmas are left alone - the shadow reads them live, same as before.
 */
void ipcc_odf_wrprotect_target(struct mm_struct *mm)
{
	struct ipcc_odf_wp_walk ww = { .count = 0 };
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		if (vma->vm_flags & (VM_SHARED | VM_HUGETLB | VM_PFNMAP))
			continue;
		if (!(vma->vm_flags & VM_MAYWRITE))
			continue;

		walk_page_vma(vma, &ipcc_odf_wp_ops, &ww);
	}
	mmap_read_unlock(mm);

	while (ww.count--)
		ipcc_odf_count(IPCC_ODF_WPROTECTED);
}

/*
 * The target is about to break COW on @folio. Hold it for the shadow: the
 * extra reference also denies wp_can_reuse_anon_folio(), so the target really
 * copies instead of writing in place and the fork-time contents survive here.
 */
void ipcc_odf_log(struct mm_struct *target_mm, unsigned long addr,
		  struct folio *folio)
{
	struct mm_struct *shadow_mm;
	void *old = NULL;

	if (folio_test_large(folio))
		return;

	/* Runs under the target's pte lock. Holding ipcc_shadows_lock across
	 * the store is what keeps the shadow's mm alive without a reference:
	 * ipcc_odf_unlink_shadow() takes the same lock.
	 */
	spin_lock(&target_mm->ipcc_shadows_lock);
	shadow_mm = list_first_entry_or_null(&target_mm->ipcc_shadows,
					     struct mm_struct, ipcc_shadow_node);
	if (shadow_mm) {
		folio_get(folio);
		old = xa_cmpxchg(&shadow_mm->ipcc_odf, addr >> PAGE_SHIFT,
				 NULL, folio, GFP_ATOMIC);
	}
	spin_unlock(&target_mm->ipcc_shadows_lock);

	if (!shadow_mm)
		return;

	if (xa_is_err(old)) {
		folio_put(folio);
		ipcc_odf_count(IPCC_ODF_LOG_FAILED);
	} else if (old) {
		folio_put(folio);
		ipcc_odf_count(IPCC_ODF_LOG_DUP);
	} else {
		ipcc_odf_count(IPCC_ODF_LOGGED);
	}
}

static struct folio *ipcc_odf_take_logged(struct mm_struct *shadow_mm,
					  unsigned long addr)
{
	return xa_erase(&shadow_mm->ipcc_odf, addr >> PAGE_SHIFT);
}

static struct folio *ipcc_odf_alloc(struct mm_struct *mm,
				    struct vm_area_struct *vma,
				    unsigned long addr)
{
	struct folio *folio;

	folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE, 0, vma, addr);
	if (!folio)
		return NULL;

	if (mem_cgroup_charge(folio, mm, GFP_KERNEL)) {
		folio_put(folio);
		return NULL;
	}
	folio_throttle_swaprate(folio, GFP_KERNEL);

	return folio;
}

/*
 * Copy one page out of the target. mmap_read_trylock rather than a plain
 * lock: the shadow already holds its own mmap_lock here, and failing the
 * address rather than ordering the two locks keeps that pair from ever
 * needing an order at all.
 */
static bool ipcc_odf_copy_from_target(struct mm_struct *target_mm,
				      struct vm_area_struct *vma,
				      unsigned long addr, struct folio *dst)
{
	struct vm_area_struct *target_vma;
	struct folio *src_folio;
	struct page *page;
	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte, ptent;
	bool copied = false;

	if (!mmap_read_trylock(target_mm))
		return false;

	target_vma = find_vma(target_mm, addr);
	if (!target_vma || target_vma->vm_start > addr)
		goto unlock_mm;

	pmd = mm_find_pmd(target_mm, addr);
	if (!pmd)
		goto unlock_mm;

	pte = pte_offset_map_lock(target_mm, pmd, addr, &ptl);
	if (!pte)
		goto unlock_mm;

	ptent = ptep_get(pte);
	if (!pte_present(ptent))
		goto unlock_pte;

	page = vm_normal_page(target_vma, addr, ptent);
	if (!page)
		goto unlock_pte;

	src_folio = page_folio(page);
	if (!folio_try_get(src_folio))
		goto unlock_pte;

	copied = !copy_mc_user_highpage(&dst->page, page, addr, vma);
	folio_put(src_folio);

unlock_pte:
	pte_unmap_unlock(pte, ptl);
unlock_mm:
	mmap_read_unlock(target_mm);
	return copied;
}

/*
 * A shadow touching an address its fork never populated. The contents come
 * from the log when the target already overwrote them, otherwise straight
 * from the target, which has not written this page since the fork. Either
 * way the shadow gets a private page of its own, so nothing it does later
 * can reach the target.
 */
bool ipcc_odf_populate(struct vm_fault *vmf, vm_fault_t *fault_ret)
{
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *shadow_mm = vma->vm_mm;
	struct mm_struct *target_mm = shadow_mm->ipcc_shadow_of;
	unsigned long addr = vmf->address & PAGE_MASK;
	struct folio *logged, *folio;
	vm_fault_t ret;
	pte_t entry;

	if (!target_mm || !vma_is_anonymous(vma))
		return false;

	folio = ipcc_odf_alloc(shadow_mm, vma, addr);
	if (!folio) {
		ipcc_odf_count(IPCC_ODF_NOMEM);
		return false;
	}

	logged = ipcc_odf_take_logged(shadow_mm, addr);
	if (logged) {
		copy_highpage(&folio->page, &logged->page);
		folio_put(logged);
		ipcc_odf_count(IPCC_ODF_FROM_LOG);
	} else if (ipcc_odf_copy_from_target(target_mm, vma, addr, folio)) {
		ipcc_odf_count(IPCC_ODF_FROM_TARGET);
	} else {
		folio_put(folio);
		ipcc_odf_count(IPCC_ODF_MISSED);
		return false;
	}

	ret = vmf_anon_prepare(vmf);
	if (ret) {
		folio_put(folio);
		*fault_ret = ret;
		return true;
	}

	__folio_mark_uptodate(folio);

	if (pte_alloc(shadow_mm, vmf->pmd)) {
		folio_put(folio);
		*fault_ret = VM_FAULT_OOM;
		return true;
	}

	vmf->pte = pte_offset_map_lock(shadow_mm, vmf->pmd, vmf->address,
				       &vmf->ptl);
	if (!vmf->pte) {
		folio_put(folio);
		*fault_ret = 0;
		return true;
	}

	if (!pte_none(ptep_get(vmf->pte))) {
		pte_unmap_unlock(vmf->pte, vmf->ptl);
		folio_put(folio);
		*fault_ret = 0;
		return true;
	}

	entry = folio_mk_pte(folio, vma->vm_page_prot);
	entry = pte_sw_mkyoung(entry);
	if (vma->vm_flags & VM_WRITE)
		entry = maybe_mkwrite(pte_mkdirty(entry), vma);

	inc_mm_counter(shadow_mm, MM_ANONPAGES);
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(folio, vma);
	set_pte_at(shadow_mm, vmf->address, vmf->pte, entry);
	update_mmu_cache_range(vmf, vma, vmf->address, vmf->pte, 1);

	pte_unmap_unlock(vmf->pte, vmf->ptl);

	*fault_ret = 0;
	return true;
}

static const char * const ipcc_odf_counter_names[IPCC_ODF_NR_COUNTERS] = {
	[IPCC_ODF_LOGGED]		= "logged",
	[IPCC_ODF_LOG_FAILED]		= "log_failed",
	[IPCC_ODF_LOG_DUP]		= "log_dup",
	[IPCC_ODF_DISCARDED]		= "discarded",
	[IPCC_ODF_FROM_LOG]		= "from_log",
	[IPCC_ODF_FROM_TARGET]		= "from_target",
	[IPCC_ODF_MISSED]		= "missed",
	[IPCC_ODF_NOMEM]		= "nomem",
	[IPCC_ODF_WPROTECTED]		= "wrprotected",
	[IPCC_ODF_SHADOW_LINKED]	= "shadow_linked",
	[IPCC_ODF_SHADOW_UNLINKED]	= "shadow_unlinked",
};

static int ipcc_odf_stats_show(struct seq_file *m, void *v)
{
	unsigned long total[IPCC_ODF_NR_COUNTERS] = { };
	unsigned long logged, discarded;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		const struct ipcc_odf_pcpu *p = per_cpu_ptr(&ipcc_odf_pcpu, cpu);

		for (i = 0; i < IPCC_ODF_NR_COUNTERS; i++)
			total[i] += READ_ONCE(p->c[i]);
	}

	for (i = 0; i < IPCC_ODF_NR_COUNTERS; i++)
		seq_printf(m, "%-20s %lu\n", ipcc_odf_counter_names[i], total[i]);

	logged = total[IPCC_ODF_LOGGED];
	discarded = total[IPCC_ODF_DISCARDED];
	seq_printf(m, "\ndiscard_ratio_pct    %lu\n",
		   logged ? (discarded * 100) / logged : 0);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ipcc_odf_stats);

static int ipcc_odf_enabled_get(void *data, u64 *val)
{
	*val = static_branch_unlikely(&ipcc_odf_enabled) ? 1 : 0;
	return 0;
}

static int ipcc_odf_enabled_set(void *data, u64 val)
{
	if (val)
		static_branch_enable(&ipcc_odf_enabled);
	else
		static_branch_disable(&ipcc_odf_enabled);

	pr_info("on-demand shadow fork %s\n", val ? "enabled" : "disabled");
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ipcc_odf_enabled_fops, ipcc_odf_enabled_get,
			 ipcc_odf_enabled_set, "%llu\n");

static int __init ipcc_odf_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("ipcc_odf", NULL);
	if (IS_ERR(dir))
		return 0;

	debugfs_create_file("stats", 0444, dir, NULL, &ipcc_odf_stats_fops);
	debugfs_create_file_unsafe("enabled", 0644, dir, NULL,
				   &ipcc_odf_enabled_fops);

	return 0;
}
late_initcall(ipcc_odf_debugfs_init);
