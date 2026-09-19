// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel support for scheduler IPC classes
 *
 * Copyright (c) 2023, Intel Corporation.
 *
 * Author: Ricardo Neri <ricardo.neri-calderon@linux.intel.com>
 *
 * On hybrid processors, the architecture differences between types of CPUs
 * lead to different number of retired instructions per cycle (IPC). IPCs may
 * differ further by classes of instructions.
 *
 * The scheduler assigns an IPC class to every task with arch_update_ipcc()
 * from data that hardware provides. Implement this interface for x86.
 *
 * See kernel/sched/sched.h for details.
 */

#include <linux/sched.h>

#include <asm/intel-family.h>
#include <asm/processor.h>
#include <asm/topology.h>
#include <linux/smp.h>
#include <linux/trace.h>
#include <linux/trace_printk.h>

/* Lockless: read remotely by the load balancer via ipcc_weighted_score(). */
static void update_ipcc_class_weights(struct task_struct *p, u8 class_idx)
{
	int i;

	for (i = 0; i < NR_IPC_CLASSES; i++) {
		unsigned short w = READ_ONCE(p->ipcc_class_weight[i]);

		if (i == class_idx) {
			w += (IPCC_WEIGHT_SCALE - w) >> IPCC_WEIGHT_SHIFT;
		} else if (w) {
			unsigned short dec = w >> IPCC_WEIGHT_SHIFT;

			/*
			 * Geometric decay stalls at w < 2^SHIFT (w >> SHIFT == 0),
			 * leaving a stuck residual that never fully forgets an old
			 * class. Force at least -1 so an inactive class reaches 0.
			 */
			w -= dec ? dec : 1;
		}
		WRITE_ONCE(p->ipcc_class_weight[i], w);
	}
}

/* Consecutive matching ticks before a class commits. Global, not shadow-only. */
#define CLASS_DEBOUNCER_SKIPS 2

static void debounce_and_update_class(struct task_struct *p, u8 new_ipcc)
{
	u16 debounce_skip;

	/* The class of @p changed. Only restart the debounce counter. */
	if (p->ipcc_prev != new_ipcc) {
		p->ipcc_stable_count = 1;
		p->ipcc_confirm_count = 0;
		goto out;
	}

	debounce_skip = p->ipcc_stable_count + 1;
	if (debounce_skip < CLASS_DEBOUNCER_SKIPS) {
		p->ipcc_stable_count++;
	} else {
		//trace_printk("ipcc-real: pid=%d class=%d cpu=%d\n",
		//	     p->pid, new_ipcc, smp_processor_id());
		p->ipcc = new_ipcc;
		update_ipcc_class_weights(p, new_ipcc);
		if (p->ipcc_confirm_count < U8_MAX)
			p->ipcc_confirm_count++;
	}

out:
	p->ipcc_prev = new_ipcc;
}

static bool classification_is_accurate(u8 hfi_class, bool smt_siblings_idle)
{
	switch (boot_cpu_data.x86_model) {
	case INTEL_FAM6_ALDERLAKE:
	case INTEL_FAM6_ALDERLAKE_L:
	case INTEL_FAM6_RAPTORLAKE:
	case INTEL_FAM6_RAPTORLAKE_P:
	case INTEL_FAM6_RAPTORLAKE_S:
	case INTEL_FAM6_ARROWLAKE:
	case INTEL_FAM6_ARROWLAKE_U:
	case INTEL_FAM6_ARROWLAKE_H: 
		if (hfi_class == 3 || hfi_class == 2 || smt_siblings_idle)
			return true;

		return false;

	default:
		return false;
	}
}

/*
 * True cpu-type check (Atom vs Core), not an HFI score comparison: P-cores
 * can legitimately have different peak scores from each other (favored-core
 * binning), so "does this cpu's score match the system's top score" is not
 * a reliable way to tell a P-core from an E-core. Exported for
 * kernel/sched/fair.c, which is generic scheduler code and has no business
 * doing its own CPUID-level topology checks.
 */
bool ipcc_cpu_is_ecore(int cpu)
{
	return (cpu_data(cpu).topo.cpu_type >> 24) == INTEL_CPU_TYPE_ATOM;
}

/*
 * __max_threads_per_core is __ro_after_init, set once from raw APIC-ID
 * topology parsing before nosmt/nosmt=force decide which siblings actually
 * boot - unlike cpu_smt_possible(), it doesn't collapse to "no SMT" just
 * because policy disabled it. See context.md.
 */
int ipcc_max_smt_threads(void)
{
	return __max_threads_per_core;
}

void intel_update_ipcc(struct task_struct *curr)
{
	u8 hfi_class;
	bool idle;
	int cpu = task_cpu(curr);

	/* E-cores don't report HFI classes. cpu_type's core bits are 31:24. */
	if (ipcc_cpu_is_ecore(cpu)) {
#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
		ipcc_classify_tick(curr, cpu);
#endif
		return;
	}

#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
	/* A shadow's tick bypasses the debouncer: one unfiltered reading. */
	if (test_task_syscall_work(curr, IPCC_SHADOW)) {
		intel_classify_ipcc_final(curr);
		if (curr->ipcc)
			ipcc_shadow_confirmed(curr);
		return;
	}
#endif

	if (intel_hfi_read_classid(&hfi_class))
		return;

	idle = sched_smt_siblings_idle(cpu);

	if (classification_is_accurate(hfi_class, idle))
		debounce_and_update_class(curr, hfi_class + 1);
}

#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
/*
 * Take a shadow's one and only classification reading, right before it dies.
 * Called from ipcc_shadow_syscall_denied(). See context.md.
 */
void intel_classify_ipcc_final(struct task_struct *p)
{
	u8 hfi_class, new_ipcc;
	bool idle;
	int cpu = task_cpu(p);

	if (intel_hfi_read_classid(&hfi_class))
		return;

	idle = sched_smt_siblings_idle(cpu);
	if (!classification_is_accurate(hfi_class, idle))
		return;

	new_ipcc = hfi_class + 1;

	if (p->ipcc != new_ipcc)
		p->ipcc_confirm_count = 0;

	p->ipcc = new_ipcc;
	update_ipcc_class_weights(p, new_ipcc);
	if (p->ipcc_confirm_count < U8_MAX)
		p->ipcc_confirm_count++;
}
#endif
