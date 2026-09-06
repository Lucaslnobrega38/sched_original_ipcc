// SPDX-License-Identifier: GPL-2.0-only
/*
 * Deferred COW for IPC-class shadow clones.
 *
 * See include/linux/ipcc_stash.h for what this mechanism is and why.
 *
 * This file owns the mm <-> mm linkage between a shadow clone and the real
 * task it was cloned from, plus the observability needed to tell whether the
 * mechanism is actually paying for itself.
 */

#define pr_fmt(fmt) "ipcc-stash: " fmt

#include <linux/debugfs.h>
#include <linux/ipcc_stash.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

DEFINE_PER_CPU(struct ipcc_stash_pcpu, ipcc_stash_pcpu);

/*
 * Off by default even when built in: the deferred path reaches into another
 * mm's page tables, so it stays inert until deliberately enabled, and can be
 * flipped back off without a reboot to A/B it against stock COW.
 */
DEFINE_STATIC_KEY_FALSE(ipcc_stash_enabled);

void ipcc_stash_mm_init(struct mm_struct *mm)
{
	INIT_LIST_HEAD(&mm->ipcc_shadows);
	spin_lock_init(&mm->ipcc_shadows_lock);
	INIT_LIST_HEAD(&mm->ipcc_shadow_node);
	mm->ipcc_shadow_of = NULL;
}

/**
 * ipcc_stash_link_shadow - record @shadow_mm as a shadow of @target_mm
 * @shadow_mm: mm of the newly forked shadow clone
 * @target_mm: mm of the real task it was cloned from
 *
 * Called from shadow_kernel_clone() before the shadow is woken, so the shadow
 * is not yet running anywhere and cannot race its own linkage.
 *
 * Takes an mmgrab() (mm_count) on @target_mm rather than an mmget()
 * (mm_users): what has to stay allocated is the target's mm_struct itself, so
 * that ipcc_stash_unlink_shadow() can still reach ->ipcc_shadows_lock even if
 * the target exited first. Pinning mm_users instead would keep the target's
 * entire address space alive for as long as a shadow lingered, which is
 * exactly what a throwaway clone must not do.
 */
void ipcc_stash_link_shadow(struct mm_struct *shadow_mm,
			    struct mm_struct *target_mm)
{
	if (WARN_ON_ONCE(shadow_mm == target_mm))
		return;

	/*
	 * A shadow can never itself be a target: it is killed on its first
	 * syscall (SYSCALL_WORK_IPCC_SHADOW, see syscall_trace_enter()), so it
	 * can never reach clone(). If this fires, that invariant broke and the
	 * list could form a chain this code does not handle.
	 */
	if (WARN_ON_ONCE(target_mm->ipcc_shadow_of))
		return;

	mmgrab(target_mm);
	shadow_mm->ipcc_shadow_of = target_mm;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_add(&shadow_mm->ipcc_shadow_node, &target_mm->ipcc_shadows);
	spin_unlock(&target_mm->ipcc_shadows_lock);

	ipcc_stash_count(IPCC_STASH_SHADOW_LINKED);
}

/**
 * ipcc_stash_unlink_shadow - detach a dying shadow's mm from its target
 * @mm: mm being torn down
 *
 * Called from exit_mmap(), which is the one path every shadow death funnels
 * through no matter how it died. That matters: of the four ways a shadow can
 * die, only one goes through ipcc_shadow_syscall_denied() - the other three
 * (ipcc_evict(), the lost-race cleanup in ipcc_shadow_fork_work(), and the
 * end-of-dwell reap in ipcc_classify_dwell()) just send SIGKILL, and a
 * SIGKILLed task exits via the signal path, never touching the syscall trap.
 *
 * A no-op for every mm that is not a shadow's, which is all of them but a
 * handful.
 */
void ipcc_stash_unlink_shadow(struct mm_struct *mm)
{
	struct mm_struct *target_mm = mm->ipcc_shadow_of;

	if (!target_mm)
		return;

	spin_lock(&target_mm->ipcc_shadows_lock);
	list_del_init(&mm->ipcc_shadow_node);
	spin_unlock(&target_mm->ipcc_shadows_lock);

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

	/*
	 * The number that decides whether any of this was worth it: what
	 * fraction of preserved snapshots were thrown away untouched. Every
	 * one of those is a full page allocation + rmap + LRU insertion the
	 * real task did not have to pay for. If this is low, the mechanism is
	 * only moving the cost around rather than avoiding it.
	 */
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
	/*
	 * _unsafe is the correct pairing for DEFINE_DEBUGFS_ATTRIBUTE (it
	 * omits the file-removal protection that attribute already forgoes).
	 * Safe here regardless: these files are created once at boot and are
	 * never removed.
	 */
	debugfs_create_file_unsafe("enabled", 0644, dir, NULL,
				   &ipcc_stash_enabled_fops);

	return 0;
}
late_initcall(ipcc_stash_debugfs_init);
