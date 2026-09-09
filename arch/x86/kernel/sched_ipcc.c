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

/*
 * Runs from sched_tick(), before rq_lock is taken (see arch_update_ipcc()'s
 * call site in kernel/sched/core.c) - there is no lock held on either side
 * of p->ipcc_class_weight[] here: the load balancer reads it from a remote
 * cpu via ipcc_weighted_score() (kernel/sched/sched.h) without taking this
 * task's rq lock either, that being the whole point of it (locking every
 * candidate rq during periodic balance scanning would be prohibitively
 * expensive - the same reason stock cpu_load()/load_avg is read lockless
 * with READ_ONCE elsewhere in fair.c). READ_ONCE/WRITE_ONCE here is what
 * that side already assumes.
 */
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

/*
 * Consecutive matching readings before a class is committed. Upstream used 4;
 * this is 2 deliberately.
 *
 * Two reasons. First, the debouncer is no longer the only filter: every commit
 * feeds update_ipcc_class_weights(), and the weight vector is itself an EWMA
 * that the load balancer reads instead of the raw class (ipcc_weighted_score(),
 * kernel/sched/sched.h). Filtering twice mostly costs responsiveness. Second,
 * a shadow clone only exists for IPCC_SHADOW_DWELL_MS, so a 4-tick floor spends
 * a large fraction of the whole observation window on debounce alone, and the
 * evidence it is being weighed against - intel_classify_ipcc_final()'s single
 * unfiltered read - is far less filtered than that anyway. The active
 * classifier is a deliberately low-fidelity, fast-turnaround sampler; holding
 * one of its two paths to a much stricter standard than the other just makes
 * the strict path unreachable.
 *
 * This is global: it changes commit latency for ordinary P-core-resident tasks
 * too, not only shadows.
 */
#define CLASS_DEBOUNCER_SKIPS 2

/**
 * debounce_and_update_class() - Process and update a task's classification
 *
 * @p:		The task of which the classification will be updated
 * @new_ipcc:	The new IPC classification
 *
 * Update the classification of @p with the new value that hardware provides.
 * Only update the classification of @p if it has been the same during
 * CLASS_DEBOUNCER_SKIPS consecutive ticks.
 */
static void debounce_and_update_class(struct task_struct *p, u8 new_ipcc)
{
	u16 debounce_skip;

	/* The class of @p changed. Only restart the debounce counter. */
	if (p->ipcc_prev != new_ipcc) {
		p->ipcc_stable_count = 1;
		p->ipcc_confirm_count = 0;
		goto out;
	}

	/*
	 * The class of @p did not change. Update it if it has been the same
	 * for CLASS_DEBOUNCER_SKIPS user ticks.
	 */
	debounce_skip = p->ipcc_stable_count + 1;
	if (debounce_skip < CLASS_DEBOUNCER_SKIPS) {
		p->ipcc_stable_count++;
	} else {
		/*
		 * Fires once per real commit, not per tick - low volume even
		 * under load. Used to measure real-P-core classification
		 * latency for comparison against the shadow mechanism's
		 * inject/fork/wake/dwell breakdown (see ipcc_classify_dwell()
		 * in arch/x86/kernel/sched_ipcc_classifier.c).
		 */
		// trace_printk("ipcc-real: pid=%d class=%d cpu=%d\n",
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

void intel_update_ipcc(struct task_struct *curr)
{
	u8 hfi_class;
	bool idle;
	int cpu = task_cpu(curr);

	/*
	 * E-cores (Atom) do not classify; only P-cores report HFI classes.
	 *
	 * topo.cpu_type is the raw, unmasked EAX from CPUID leaf 0x1A (see
	 * arch/x86/kernel/cpu/topology_common.c): the core-type value
	 * (INTEL_CPU_TYPE_ATOM/_CORE) is only bits 31:24, bits 23:0 carry a
	 * non-zero model-specific ID, so it must be shifted before comparing.
	 */
	if ((cpu_data(cpu).topo.cpu_type >> 24) == INTEL_CPU_TYPE_ATOM) {
#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
		ipcc_classify_tick(curr, cpu);
#endif
		return;
	}

#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
	/*
	 * A shadow's own tick bypasses the debouncer entirely: it gets exactly
	 * one unfiltered reading, and that reading - not two consecutive
	 * matching ones - is the whole sample. See intel_classify_ipcc_final()'s
	 * doc comment for why a debounced multi-tick reading would not mean
	 * anything more here anyway, and ipcc_shadow_confirmed() for why this
	 * lets ipcc_classify_dwell() stop watching immediately instead of
	 * always spending the full IPCC_SHADOW_DWELL_MS budget.
	 */
	if (test_task_syscall_work(curr, IPCC_SHADOW)) {
		intel_classify_ipcc_final(curr);
		if (curr->ipcc)
			ipcc_shadow_confirmed(curr);
		return;
	}
#endif

	if (intel_hfi_read_classid(&hfi_class))
		return;

	/*
	 * 0 is a valid classification for Intel Thread Director. A scheduler
	 * IPCC class of 0 means that the task is unclassified. Adjust.
	 */
	idle = sched_smt_siblings_idle(cpu);

	if (classification_is_accurate(hfi_class, idle))
		debounce_and_update_class(curr, hfi_class + 1);
}

#ifdef CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER
/**
 * intel_classify_ipcc_final() - Take a shadow's last possible classification
 * reading before it dies
 *
 * @p:	The shadow task about to be killed
 *
 * debounce_and_update_class() only ever commits on the *second* consecutive
 * matching tick-driven reading - fine for a task that keeps getting sampled,
 * useless for a shadow that is about to die on its first syscall and has no
 * next tick to supply that second reading. Called from
 * ipcc_shadow_syscall_denied() (sched_ipcc_classifier.c) right before
 * do_exit(), so this call is provably @p's last chance: a single valid,
 * model-gated reading taken here is committed immediately instead of being
 * sent through the debounce path to be dropped on the floor.
 *
 * Still subject to the one constraint debounce was never the cause of:
 * MSR_IA32_HW_FEEDBACK_CHAR's valid bit is not set until Thread Director has
 * observed the thread for its own hardware warmup period (see
 * IPCC_SHADOW_DWELL_MS's comment in sched_ipcc_classifier.c). A shadow that
 * died before the hardware had time to form an opinion at all still reads
 * invalid here - asking sooner cannot manufacture an observation that was
 * never made.
 *
 * Counted as exactly one tick's worth of evidence, incrementing
 * ipcc_confirm_count the same way debounce_and_update_class()'s commit branch
 * does. That count is what ipcc_blend_class_weight() (sched_ipcc_classifier.c)
 * uses to decide how far to move the *target's* vector, so the unfiltered
 * reading taken here has to declare its own weight honestly: it represents
 * one sub-millisecond sliver of observation, not the handful of confirmed
 * ticks a full dwell accumulates. Leaving the count at zero - as this did before -
 * made the blend a no-op, so a short-lived shadow cost a real copy_process()
 * and a classifier turn to deliver nothing at all.
 *
 * Deliberately not rate-limited. It used to be, on the grounds that a queued
 * shadow could die on its first syscall at any moment and so this was
 * reachable from every managed E-core at once. That is no longer true:
 * shadows are now handed back from shadow_kernel_clone() parked in TASK_NEW
 * and only woken for their dwell turn, so a queued shadow has never executed
 * a userspace instruction and cannot reach a syscall; evicted ones die by
 * SIGKILL through the signal path, never through the trap. Only the single
 * dwelling shadow gets here, once, right before it exits - there is no
 * fan-in left to cap, and a system-wide limiter would now silently discard
 * one shadow's only sample because of what an unrelated shadow did.
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

	/*
	 * A reading that disagrees with what was committed so far invalidates
	 * the confirmations behind it, exactly as the debouncer's class-change
	 * branch does - those ticks were evidence for a different class.
	 */
	if (p->ipcc != new_ipcc)
		p->ipcc_confirm_count = 0;

	p->ipcc = new_ipcc;
	update_ipcc_class_weights(p, new_ipcc);
	if (p->ipcc_confirm_count < U8_MAX)
		p->ipcc_confirm_count++;
}
#endif
