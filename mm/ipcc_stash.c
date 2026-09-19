// SPDX-License-Identifier: GPL-2.0-only
/*
 * Deferred COW for IPC-class shadow clones. See include/linux/ipcc_stash.h
 * and context.md for the design. This file owns the mm<->mm linkage between
 * a shadow and its target, plus observability.
 */

#define pr_fmt(fmt) "ipcc-stash: " fmt

#include <linux/debugfs.h>
#include <linux/ipcc_stash.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pagewalk.h>
#include <linux/rmap.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/swapops.h>

DEFINE_PER_CPU(struct ipcc_stash_pcpu, ipcc_stash_pcpu);

/* Off by default even when built in - see context.md. */
DEFINE_STATIC_KEY_FALSE(ipcc_stash_enabled);

void ipcc_stash_mm_init(struct mm_struct *mm)
{
	INIT_LIST_HEAD(&mm->ipcc_shadows);
	spin_lock_init(&mm->ipcc_shadows_lock);
	INIT_LIST_HEAD(&mm->ipcc_shadow_node);
	mm->ipcc_shadow_of = NULL;
	INIT_LIST_HEAD(&mm->ipcc_stash);
}

/* Free every stash entry on @mm's list. Only safe once @mm is off its
 * target's ipcc_shadows list - see ipcc_stash_unlink_shadow().
 */
static void ipcc_stash_drain(struct mm_struct *mm)
{
	struct ipcc_stash_entry *e, *tmp;

	list_for_each_entry_safe(e, tmp, &mm->ipcc_stash, node) {
		list_del(&e->node);
		folio_put(e->folio);
		kfree(e);
		ipcc_stash_count(IPCC_STASH_DISCARDED);
	}
}

struct ipcc_arm_walk {
	struct mmu_notifier_range range;
	unsigned long armed;
};

/* Replace one present pte with a stash marker. Modelled on __uprobe_write()
 * and the device-private arm of zap_nonpresent_ptes(). Only present ptes are
 * touched, so untouched addresses keep faulting the ordinary way.
 */
static int ipcc_stash_arm_pte(pte_t *pte, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	struct ipcc_arm_walk *aw = walk->private;
	struct vm_area_struct *vma = walk->vma;
	pte_t ptent = ptep_get(pte);
	struct folio *folio;
	struct page *page;

	if (!pte_present(ptent))
		return 0;

	page = vm_normal_page(vma, addr, ptent);
	if (!page)
		return 0;
	folio = page_folio(page);

	/* Large folios left mapped - see context.md. */
	if (folio_test_large(folio)) {
		ipcc_stash_count(IPCC_STASH_DECLINED_LARGE);
		return 0;
	}

	ptep_clear_flush(vma, addr, pte);
	folio_remove_rmap_pte(folio, page, vma);
	dec_mm_counter(walk->mm, mm_counter(folio));
	folio_put(folio);

	set_pte_at(walk->mm, addr, pte, make_pte_marker(PTE_MARKER_IPCC_STASH));
	aw->armed++;

	return 0;
}

static int ipcc_stash_arm_pre_vma(unsigned long start, unsigned long end,
				  struct mm_walk *walk)
{
	struct ipcc_arm_walk *aw = walk->private;

	mmu_notifier_range_init(&aw->range, MMU_NOTIFY_CLEAR, 0, walk->mm,
				start, end);
	mmu_notifier_invalidate_range_start(&aw->range);

	return 0;
}

static void ipcc_stash_arm_post_vma(struct mm_walk *walk)
{
	struct ipcc_arm_walk *aw = walk->private;

	mmu_notifier_invalidate_range_end(&aw->range);
}

static const struct mm_walk_ops ipcc_stash_arm_ops = {
	.pte_entry	= ipcc_stash_arm_pte,
	.pre_vma	= ipcc_stash_arm_pre_vma,
	.post_vma	= ipcc_stash_arm_post_vma,
	.walk_lock	= PGWALK_WRLOCK,
};

/* Trap a shadow's every access (read or write) to a defanged shared vma -
 * write-protect alone would still let reads through to the live page. Called
 * with the shadow's mmap_lock held for write, before it has ever run.
 */
void ipcc_stash_arm_vma(struct vm_area_struct *vma)
{
	struct ipcc_arm_walk aw = { .armed = 0 };

	mmap_assert_write_locked(vma->vm_mm);
	WARN_ON(walk_page_vma(vma, &ipcc_stash_arm_ops, &aw));
}

/*
 * Hand a page's pre-write contents to @target_mm's shadow. Called from the
 * target's own write fault, no locks held, folio already allocated and
 * filled. Returns true if the stash took ownership of @folio.
 *
 * Everything happens under the target's ipcc_shadows_lock, which
 * ipcc_stash_unlink_shadow() also takes to remove a shadow from the list -
 * that is what makes this safe without a reference on the shadow's mm.
 *
 * An entry already present for @addr means the target already wrote this
 * page since the fork, so the existing (older, correct) entry must stay.
 */
bool ipcc_stash_offer(struct mm_struct *target_mm, unsigned long addr,
		      struct folio *folio)
{
	struct ipcc_stash_entry *e, *new;
	struct mm_struct *shadow_mm;
	bool stored = false;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new) {
		ipcc_stash_count(IPCC_STASH_DECLINED_NOMEM);
		return false;
	}

	spin_lock(&target_mm->ipcc_shadows_lock);

	shadow_mm = list_first_entry_or_null(&target_mm->ipcc_shadows,
					     struct mm_struct, ipcc_shadow_node);
	if (!shadow_mm) {
		ipcc_stash_count(IPCC_STASH_DECLINED_MMGONE);
		goto out;
	}

	list_for_each_entry(e, &shadow_mm->ipcc_stash, node) {
		if (e->addr == addr) {
			ipcc_stash_count(IPCC_STASH_DECLINED_OTHER);
			goto out;
		}
	}

	new->addr = addr;
	new->folio = folio;
	list_add(&new->node, &shadow_mm->ipcc_stash);
	new = NULL;
	stored = true;
	ipcc_stash_count(IPCC_STASH_CREATED);

out:
	spin_unlock(&target_mm->ipcc_shadows_lock);
	kfree(new);
	return stored;
}

/*
 * Claim the stashed contents for @addr, if any. Called from the shadow's own
 * fault on a PTE_MARKER_IPCC_STASH marker. Returns the folio (reference
 * transferred), or NULL if the target never wrote this page.
 *
 * Reaches the lock through ->ipcc_shadow_of, which holds an mmgrab on the
 * target so the lock outlives it. A NULL ->ipcc_shadow_of means a race with
 * teardown; NULL here is correct either way.
 */
struct folio *ipcc_stash_take(struct mm_struct *shadow_mm, unsigned long addr)
{
	struct mm_struct *target_mm = shadow_mm->ipcc_shadow_of;
	struct ipcc_stash_entry *e;
	struct folio *folio = NULL;

	if (!target_mm)
		return NULL;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_for_each_entry(e, &shadow_mm->ipcc_stash, node) {
		if (e->addr == addr) {
			list_del(&e->node);
			folio = e->folio;
			kfree(e);
			ipcc_stash_count(IPCC_STASH_CONSUMED);
			break;
		}
	}
	spin_unlock(&target_mm->ipcc_shadows_lock);

	return folio;
}

/* Record @shadow_mm as a shadow of @target_mm. Called from
 * shadow_kernel_clone() before the shadow is woken. mmgrab (not mmget) on
 * @target_mm: keeps the mm_struct itself alive, not its whole address space.
 */
void ipcc_stash_link_shadow(struct mm_struct *shadow_mm,
			    struct mm_struct *target_mm)
{
	if (WARN_ON_ONCE(shadow_mm == target_mm))
		return;

	/* A shadow can never itself be a target - see context.md. */
	if (WARN_ON_ONCE(target_mm->ipcc_shadow_of))
		return;

	mmgrab(target_mm);
	shadow_mm->ipcc_shadow_of = target_mm;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_add(&shadow_mm->ipcc_shadow_node, &target_mm->ipcc_shadows);
	spin_unlock(&target_mm->ipcc_shadows_lock);

	ipcc_stash_count(IPCC_STASH_SHADOW_LINKED);
}

/* Detach a dying shadow's mm from its target. Called from exit_mmap(), the
 * one path every shadow death funnels through regardless of how it died.
 * No-op for any mm that isn't a shadow's.
 */
void ipcc_stash_unlink_shadow(struct mm_struct *mm)
{
	struct mm_struct *target_mm = mm->ipcc_shadow_of;

	if (!target_mm)
		return;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_del_init(&mm->ipcc_shadow_node);
	spin_unlock(&target_mm->ipcc_shadows_lock);

	/* Drain only after unlink - see ipcc_stash_offer(). */
	ipcc_stash_drain(mm);

	mm->ipcc_shadow_of = NULL;
	mmdrop(target_mm);

	ipcc_stash_count(IPCC_STASH_SHADOW_UNLINKED);
}

static const char * const ipcc_stash_counter_names[IPCC_STASH_NR_COUNTERS] = {
	[IPCC_STASH_CREATED]		= "created",
	[IPCC_STASH_CONSUMED]		= "consumed",
	[IPCC_STASH_DISCARDED]		= "discarded",
	[IPCC_STASH_DECLINED_PINNED]	= "declined_pinned",
	[IPCC_STASH_DECLINED_LARGE]	= "declined_large",
	[IPCC_STASH_DECLINED_VMALOCK]	= "declined_vmalock",
	[IPCC_STASH_DECLINED_MMGONE]	= "declined_mmgone",
	[IPCC_STASH_DECLINED_NOMEM]	= "declined_nomem",
	[IPCC_STASH_DECLINED_OTHER]	= "declined_other",
	[IPCC_STASH_PARENT_WP_COPY]	= "parent_wp_copy",
	[IPCC_STASH_SHADOW_LINKED]	= "shadow_linked",
	[IPCC_STASH_SHADOW_UNLINKED]	= "shadow_unlinked",
};

static int ipcc_stash_stats_show(struct seq_file *m, void *v)
{
	unsigned long total[IPCC_STASH_NR_COUNTERS] = { };
	unsigned long created, discarded;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		const struct ipcc_stash_pcpu *p = per_cpu_ptr(&ipcc_stash_pcpu, cpu);

		for (i = 0; i < IPCC_STASH_NR_COUNTERS; i++)
			total[i] += READ_ONCE(p->c[i]);
	}

	for (i = 0; i < IPCC_STASH_NR_COUNTERS; i++)
		seq_printf(m, "%-20s %lu\n", ipcc_stash_counter_names[i], total[i]);

	/* Fraction of snapshots thrown away untouched - see context.md. */
	created = total[IPCC_STASH_CREATED];
	discarded = total[IPCC_STASH_DISCARDED];
	seq_printf(m, "\ndiscard_ratio_pct    %lu\n",
		   created ? (discarded * 100) / created : 0);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ipcc_stash_stats);

static int ipcc_stash_enabled_get(void *data, u64 *val)
{
	*val = static_branch_unlikely(&ipcc_stash_enabled) ? 1 : 0;
	return 0;
}

static int ipcc_stash_enabled_set(void *data, u64 val)
{
	if (val)
		static_branch_enable(&ipcc_stash_enabled);
	else
		static_branch_disable(&ipcc_stash_enabled);

	pr_info("deferred COW %s\n", val ? "enabled" : "disabled");
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ipcc_stash_enabled_fops, ipcc_stash_enabled_get,
			 ipcc_stash_enabled_set, "%llu\n");

static int __init ipcc_stash_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("ipcc_stash", NULL);
	if (IS_ERR(dir))
		return 0;

	debugfs_create_file("stats", 0444, dir, NULL, &ipcc_stash_stats_fops);
	/* _unsafe: correct pairing for DEFINE_DEBUGFS_ATTRIBUTE; these files
	 * are created once at boot and never removed.
	 */
	debugfs_create_file_unsafe("enabled", 0644, dir, NULL,
				   &ipcc_stash_enabled_fops);

	return 0;
}
late_initcall(ipcc_stash_debugfs_init);
