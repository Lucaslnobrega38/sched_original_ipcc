// SPDX-License-Identifier: GPL-2.0-only
/*
 * Active IPC-class classification of E-core-resident tasks.
 *
 * Intel Thread Director only classifies tasks while they run on a P-core;
 * E-cores report nothing usable. A task that is born on an E-core and never
 * leaves it is therefore never classified, stays at the neutral fallback
 * score forever, and is invisible to IPC-class-aware placement - which is
 * precisely the task that would benefit most from being moved.
 *
 * This classifies such tasks actively, without ever taking them off their
 * real work: for a target task X, a throwaway COW shadow clone of X is
 * created (X itself is untouched and keeps running on its E-core), the shadow
 * is pinned to a dedicated P-core so the hardware can classify it, and once a
 * class is established it is copied back onto X and the shadow is destroyed.
 *
 * The shadow can never produce a side effect: it carries
 * SYSCALL_WORK_IPCC_SHADOW, so its first syscall attempt kills it (see
 * syscall_trace_enter()), and everything it writes to memory is private COW.
 *
 * See kernel/fork.c:shadow_kernel_clone() for the fork itself.
 */

#define pr_fmt(fmt) "ipcc-classifier: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpuset.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/sched/isolation.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/trace_printk.h>
#include <linux/wait.h>

#include <asm/intel-family.h>
#include <asm/processor.h>
#include <asm/topology.h>

/*
 * cpu_data(cpu)->topo.cpu_type stores the raw, unmasked EAX from CPUID leaf
 * 0x1A (see arch/x86/kernel/cpu/topology_common.c) - the core-type value
 * (INTEL_CPU_TYPE_ATOM / _CORE) only occupies bits 31:24; bits 23:0 carry a
 * model-specific "native model ID" that is not zero. Comparing the raw value
 * directly against INTEL_CPU_TYPE_ATOM therefore never matches. This is the
 * single point that shifts it correctly; every E-core/P-core check in this
 * file goes through here.
 */
static inline bool ipcc_cpu_is_atom(int cpu)
{
	return (cpu_data(cpu).topo.cpu_type >> 24) == INTEL_CPU_TYPE_ATOM;
}

/*
 * How long a shadow is allowed to live while being classified. A budget, not a
 * duration: the dwell ends early when the shadow dies on its own, so this only
 * bounds a shadow that never makes a syscall.
 *
 * Floor, and it is a hard one: debounce_and_update_class() (sched_ipcc.c)
 * commits only after CLASS_DEBOUNCER_SKIPS consecutive matching readings, and
 * for a shadow that never syscalls the debouncer is the *only* path to a class.
 * intel_classify_ipcc_final() cannot rescue that case - it reads
 * MSR_IA32_HW_FEEDBACK_CHAR via rdmsrl(), which reports the logical processor
 * the caller is running on, so it is only meaningful from inside the shadow's
 * own context, which is exactly why it is reached from the syscall trap and
 * nowhere else. Calling it from the reaper would read the reaper's cpu. So at
 * HZ ticks per second, this must exceed CLASS_DEBOUNCER_SKIPS tick periods by
 * enough margin to be worth forking for at all.
 *
 * There is a second floor that is *not* quantified here: the hardware does not
 * set the valid bit in MSR_IA32_HW_FEEDBACK_CHAR until Thread Director has
 * observed the thread for some period of its own (intel_hfi_read_classid()
 * returns -EINVAL until then), and both classification paths go through that
 * read. Nothing in the tree documents how long that takes - not
 * drivers/thermal/intel/intel_hfi.c, not Documentation/arch/x86/intel-hfi.rst -
 * so it has to be measured rather than assumed. The k field in
 * ipcc_classify_dwell()'s trace line is what measures it: "timeout" lines with
 * k=0 mean this budget expires before the hardware ever produces a valid
 * reading, and no amount of shortening helps; a healthy k means there is real
 * headroom. (An earlier version of this comment asserted a specific ">100ms on
 * Raptor Lake" figure. It had no source and is removed.)
 */
#define IPCC_SHADOW_DWELL_MS		10

/*
 * How long a pending (not yet forked) slot occupant keeps its seniority veto
 * in ipcc_classify_submit(). Without an upper bound, a target that submits
 * and then blocks in a long syscall before its task_work can run would hold
 * its E-core's slot indefinitely, since every later candidate has by
 * definition been served more recently than it has. Generous on purpose: a
 * legitimately waiting occupant must survive several full round-robin laps
 * (ipcc_num_ecores * ~IPCC_SHADOW_DWELL_MS each) before anyone may take over.
 */
#define IPCC_SLOT_STALE_MS		2000

/*
 * Minimum lag (ktime_get() - ipcc_shadow_last_turn) a task must have built up
 * before it is even offered for a turn, checked in ipcc_classify_eligible()
 * before anything else is spent on it. A turn costs a real copy_process()
 * plus up to IPCC_SHADOW_DWELL_MS of a dedicated core, so this is the single
 * throttle that has to carry two, otherwise unrelated, jobs:
 *
 *  - Freshly forked tasks start at lag 0 (see __sched_fork()) and the machine
 *    churns these out constantly (build steps, shell helpers, short-lived
 *    unit jobs). Without a floor here they would need no wait at all, and -
 *    worse - under the old turn-counter scheme a never-served task sorted
 *    *ahead* of everyone: observed swamping every slot with fresh pids whose
 *    shadows died 22-800us later, having classified nothing.
 *
 *  - A task that just consumed a turn also resets to lag 0, and an
 *    event-loop daemon (dbus, journald, systemd-resolved: wakes hundreds of
 *    times per round-robin lap, blocks again in microseconds) can otherwise
 *    walk right back into the next slot that happens to free up - it isn't
 *    evicting anyone there is simply no contender at that instant - and keep
 *    doing so indefinitely. Observed: a single such daemon as the target in
 *    nearly every cycle for minutes, a compute-bound task that would have
 *    actually classified getting a turn roughly once every five minutes.
 *
 * Same floor serves both because both are "insufficiently rested" by the
 * same measure; splitting them would need two independent thresholds for no
 * real gain.
 *
 * Lowered to match one tick (1ms @ HZ=1000, the fastest this can possibly be
 * re-checked anyway) after re-measuring the failure mode above under the
 * current ktime-lag scheme rather than the old turn-counter one it was
 * originally observed on. Four processes spamming a fast syscall in a tight
 * loop, alongside one real CPU-bound target, still took ~93% of all turns -
 * but the target's own share (~7%) was still ~100 turns/s, converging its
 * weight vector to fully saturated in under 150ms and staying there. Being
 * outvoted for slots no longer means being starved of turns: with no per-slot
 * queue depth beyond one pending request, the daemons cannot accumulate a
 * backlog that blocks the target, only win the immediate race for a slot
 * that just freed. Aggregate throughput also rose roughly 4-5x over the
 * 200ms floor under comparable contention.
 */
#define IPCC_MIN_LAG_MS			1

static int ipcc_classifier_cpu = -1;
static struct task_struct *ipcc_reaper;
static struct freq_qos_request ipcc_freq_req;

/*
 * Exposed so sched_ipcc.c can log every raw classification attempt on the
 * classifier cpu specifically (see intel_update_ipcc()), without spamming
 * for ordinary P-core activity elsewhere. Diagnostic only.
 */
int ipcc_get_classifier_cpu(void)
{
	return ipcc_classifier_cpu;
}

/*
 * The fork has to happen in the target's own context, so it is injected as a
 * task_work and the result comes back through here. Heap-allocated fresh per
 * attempt: many attempts can be forked and sitting in ipcc_queue at once
 * (see below), each needing its own storage.
 */
struct ipcc_shadow_req {
	struct callback_head	work;
	struct task_struct	*target;	/* ref held; who to copy the class back to */
	struct task_struct	*shadow;	/* ref held once forked; NULL until delivered */
	int			error;		/* set instead of ->shadow on failure */

	/*
	 * Whoever wins the 0 -> {EVICTED,DELIVERED} CAS decides what happens
	 * next - see ipcc_shadow_fork_work() and ipcc_evict(). Replaces a
	 * plain "ready" bool because a request can now be superseded by a
	 * newer arrival on the same core's slot before its own fork even
	 * finishes, and only one side of that race ever has valid
	 * ->shadow/->error to read.
	 */
	atomic_t		state;
#define IPCC_REQ_EVICTED	1
#define IPCC_REQ_DELIVERED	2

	/*
	 * Profiling timestamps, one per cycle stage. t_inject is set at
	 * submission, right before task_work_add(); the rest are set inside
	 * ipcc_shadow_fork_work(), which runs in the *target's* context - so
	 * these are written by a different task than the one that eventually
	 * reads them (ipcc_reaper, in ipcc_classify_dwell()), but only ever
	 * after ->state reaches IPCC_REQ_DELIVERED, which is also how
	 * ipcc_reaper decides this request is worth looking at at all.
	 */
	ktime_t			t_inject;
	ktime_t			t_callback;
	ktime_t			t_forked;
};

/*
 * One slot per E-core under management, not a shared pool. A tick on E-core
 * @cpu only ever touches ipcc_queue[ipcc_cpu_to_slot[cpu]] - never anyone
 * else's slot - and always *replaces* whatever was there (see ipcc_evict()),
 * rather than being turned away when the queue is "full" the way an earlier,
 * shared 8-slot design was.
 *
 * That earlier design measurably starved new arrivals: with admission keyed
 * only by "is there a free slot / is this exact target already queued",
 * whichever handful of tasks happened to tick most reliably (a couple of hot
 * resident daemons, or a flood of long-lived test workers) could occupy
 * every slot indefinitely, since nothing ever preferred a task that had
 * never been sampled over one that had. Measured on hardware: a brand new
 * single-threaded process pinned to an otherwise-busy E-core went over 25s
 * without ever being offered a single classification attempt, while two
 * resident tasks alone absorbed 800+ admissions in the same window.
 *
 * Per-core slots fix this structurally: ipcc_reaper visits slots in a
 * round-robin sweep (see ipcc_queue_next()), so every E-core gets a turn
 * within one lap regardless of how hot any *other* core's residents are.
 * Whichever task happens to be curr on a given core when that core's slot is
 * last serviced is the one offered next - if that task is already classified
 * and a different task on the same core would rather go first, the newer
 * tick simply keeps replacing the slot's contents (see ipcc_evict()) until
 * the reaper actually gets there, so what runs is always whoever was curr
 * most recently, not whoever happened to submit first.
 *
 * As before, ipcc_reaper only ever dwells on one request at a time from its
 * single-threaded loop, which is what actually keeps two shadows from ever
 * sharing the classifier cpu (ITD classification is per-core) - the slot
 * structure here is purely about *what to try next*, not about mutual
 * exclusion on the core itself.
 */
static struct ipcc_shadow_req **ipcc_queue;	/* ipcc_num_ecores entries */
static int *ipcc_ecore_cpus;			/* ipcc_num_ecores entries: the actual cpu numbers */
static int *ipcc_cpu_to_slot;			/* nr_cpu_ids entries: reverse lookup, -1 if not managed */
static int ipcc_num_ecores;
static unsigned int ipcc_rr_cursor;		/* only ever touched by ipcc_reaper */
static DEFINE_SPINLOCK(ipcc_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(ipcc_wait);

/*
 * Only single-threaded, non-kernel tasks with a real address space can be
 * shadowed: with a shared mm (CLONE_VM) there is no COW isolation, so the
 * shadow's writes would land directly in the real address space. Used by
 * both the tick-triggered live path and the debugfs manual trigger.
 *
 * Shadows themselves are excluded, and that exclusion is load-bearing rather
 * than tidiness. A shadow is a normal single-threaded task with an mm, so it
 * passes every other test here; and until its dwell turn comes it runs on
 * whatever E-core it inherited, where it will be curr at some tick and get
 * offered as a candidate like anything else. Letting that happen means a
 * shadow forking its own shadow, recursively, each generation consuming a
 * slot and real E-core time. This was invisible while shadows were dying
 * almost immediately to a corrupted-RAX segfault (see shadow_kernel_clone()
 * in kernel/fork.c) - with that fixed they survive until their first
 * syscall, which for a compute-bound target can be a long time.
 */
static bool ipcc_classify_eligible(struct task_struct *p)
{
	if (!p->mm || (p->flags & PF_KTHREAD) || get_nr_threads(p) != 1 ||
	    test_task_syscall_work(p, IPCC_SHADOW))
		return false;

	/*
	 * Skip tasks that have not built up enough lag since their last turn
	 * (or since fork, if they have never had one) - see IPCC_MIN_LAG_MS.
	 * Wall clock, not cpu time: a task that is simply not running yet is
	 * exactly as ineligible as one that is running but was just served,
	 * and both become eligible again the same way - by waiting.
	 */
	if (ktime_before(ktime_get(),
			 ktime_add_ms(p->ipcc_shadow_last_turn, IPCC_MIN_LAG_MS)))
		return false;

	return true;
}

/*
 * Woken by a shadow that is about to die on its own (the syscall trap), so
 * ipcc_classify_dwell() can stop observing the instant there is nothing left
 * to observe instead of sleeping out the rest of its dwell budget against a
 * corpse. One shadow dwells at a time and the predicate is per-shadow, so a
 * wake that does not concern the current sleeper simply re-evaluates to false
 * and goes back to sleep.
 */
static DECLARE_WAIT_QUEUE_HEAD(ipcc_dwell_wait);

/**
 * ipcc_kill_parked_shadow - destroy a shadow that has never been woken
 *
 * A shadow is handed back from shadow_kernel_clone() parked in TASK_NEW, and
 * TASK_NEW is not in the TASK_WAKEKILL | TASK_INTERRUPTIBLE mask that
 * try_to_wake_up() tests - so a bare send_sig(SIGKILL) on one would set the
 * pending signal and leave it parked forever, never reaped. It has to be
 * woken to be able to die.
 *
 * Signal first, then wake: it comes up with a fatal signal already pending
 * and exits in ret_from_fork without executing a single userspace
 * instruction.
 *
 * Only valid for a shadow that has *not* been woken yet: wake_up_new_task()
 * is a once-per-task call (it activate_task()s onto a runqueue), not an
 * idempotent kick. That precondition holds by construction - a request is
 * consumed by ipcc_classify_dwell() or by the eviction paths, never both,
 * since ipcc_queue_next() unlinks it from the slot under ipcc_queue_lock and
 * ipcc_classify_submit() only ever finds it there under that same lock. The
 * eviction paths therefore always hold a shadow that never got its turn;
 * ipcc_classify_dwell(), which does the waking itself, uses a plain
 * send_sig() at the end instead of this.
 *
 * Safe to call with IRQs already disabled, which the eviction path from
 * ipcc_classify_tick() does: both send_sig() and wake_up_new_task() are
 * built entirely out of irqsave/irqrestore pairs, neither may sleep, and
 * with nr_cpus_allowed == 1 select_task_rq() short-circuits to
 * cpumask_any() without entering the placement heuristics.
 */
static void ipcc_kill_parked_shadow(struct task_struct *shadow)
{
	send_sig(SIGKILL, shadow, 1);
	wake_up_new_task(shadow);
}

/**
 * ipcc_shadow_syscall_denied - a shadow clone attempted a syscall
 *
 * Called from syscall_trace_enter() before any other syscall work, with the
 * syscall not yet executed. Does not return.
 *
 * A shadow only represents its parent faithfully for as long as it runs the
 * same instructions. The moment it reaches a syscall it would either cause a
 * side effect or diverge into error handling, so the sample is over. Killing
 * it here is both the isolation guarantee and the natural end of the sample;
 * it dies through the ordinary do_exit() path and is autoreaped by the
 * classifier kthread like any other shadow.
 */
void __noreturn ipcc_shadow_syscall_denied(void)
{
	/*
	 * Last chance at a reading before this task is gone for good - see
	 * intel_classify_ipcc_final()'s doc comment (sched_ipcc.c) for why
	 * this bypasses the normal tick-driven debounce path entirely rather
	 * than waiting for a next tick that will never come.
	 */
	intel_classify_ipcc_final(current);

	/*
	 * Publish the death and wake whoever is dwelling on us, so the
	 * classifier core is released the moment there is nothing left to
	 * observe rather than sleeping out the rest of its budget.
	 *
	 * Release/acquire against ipcc_classify_dwell()'s predicate: the class
	 * intel_classify_ipcc_final() just committed must be visible to anyone
	 * who observes a non-zero timestamp, since that timestamp is exactly
	 * what tells the reaper the sample is final and ready to be read.
	 */
	smp_store_release(&current->ipcc_shadow_died_at, ktime_get());
	wake_up(&ipcc_dwell_wait);

	do_exit(SIGKILL);
}

/*
 * Called from intel_update_ipcc() (sched_ipcc.c) the moment a shadow's own
 * tick produces a valid class - the counterpart to
 * ipcc_shadow_syscall_denied() above for a shadow that never makes it to a
 * syscall. Same release/acquire pairing against ipcc_classify_dwell()'s
 * predicate, same "wake the reaper now instead of sleeping out the budget"
 * reasoning.
 *
 * Guarded so only the first call stamps: intel_update_ipcc() runs on every
 * subsequent tick too for as long as the shadow keeps existing, and without
 * this it would keep overwriting the timestamp with a newer one on every
 * tick until the reaper actually wakes up and reads it.
 */
void ipcc_shadow_confirmed(struct task_struct *p)
{
	if (READ_ONCE(p->ipcc_shadow_confirmed_at))
		return;

	smp_store_release(&p->ipcc_shadow_confirmed_at, ktime_get());
	wake_up(&ipcc_dwell_wait);
}

/*
 * Runs in the target task's own context (current == target), from the
 * exit-to-usermode task_work loop: no locks held, IRQs enabled, sleepable -
 * exactly what copy_process() requires.
 */
static void ipcc_shadow_fork_work(struct callback_head *work)
{
	struct ipcc_shadow_req *req = container_of(work, struct ipcc_shadow_req, work);
	struct task_struct *shadow;

	/* Stage 1 boundary: target has reached resume_user_mode_work(). */
	req->t_callback = ktime_get();

	/*
	 * There are two ways to get here, and only one of them is the one this
	 * callback was written for.
	 *
	 * The intended one: the target came back out to userspace and ran its
	 * task_work on the way, with a fully live context.
	 *
	 * The other one: the target is *exiting*, and do_exit() is running its
	 * leftover task_work for it (exit_task_work(), kernel/exit.c) - which
	 * happens well after the context this fork needs has been torn down.
	 * By that point exit_mm(), exit_files(), exit_fs() and
	 * exit_nsproxy_namespaces() have all already run, so current->mm,
	 * ->files, ->fs and ->nsproxy are NULL. copy_files()/copy_mm() tolerate
	 * that (they treat it as the kernel-thread case), but copy_fs() does
	 * not: it calls copy_fs_struct(NULL), which dereferences old->umask
	 * unconditionally and panics. This was not theoretical - it oopsed
	 * repeatedly (copy_fs_struct+0x4f, CR2=0xc, always under
	 * do_exit->task_work_run), and mass process exit is exactly what
	 * shutdown is, which is where it showed up most.
	 *
	 * copy_process()'s own task_sigpending() check does not cover this:
	 * a task that called exit_group() itself has no pending signal at all.
	 * PF_EXITING (set by exit_signals(), long before exit_task_work()) is
	 * the reliable signal, so test it directly.
	 *
	 * A dying task is worthless as a sample anyway - it is not doing the
	 * work we wanted to characterize - so there is nothing lost in
	 * refusing. -ESRCH matches what task_work_add() reports for a target
	 * that was already too far gone to accept the request.
	 */
	if (current->flags & PF_EXITING) {
		req->error = -ESRCH;
		goto deliver;
	}

	shadow = shadow_kernel_clone(ipcc_reaper, ipcc_classifier_cpu);

	/* Stage 2 boundary: copy_process() done; shadow parked, not yet woken. */
	req->t_forked = ktime_get();

	if (IS_ERR(shadow)) {
		req->error = PTR_ERR(shadow);
	} else {
		/*
		 * The shadow comes back already bound to the classifier cpu but
		 * still in TASK_NEW - on no runqueue, costing nothing - and only
		 * starts when ipcc_classify_dwell() wakes it for its turn. See
		 * shadow_kernel_clone()'s doc comment for why the fork and the
		 * start are split like this.
		 */
		req->shadow = shadow;

		/*
		 * current == target here. Only a fork that got this far counts
		 * as a consumed turn: this is the point past which the target
		 * has actually cost the classifier a real copy_process() and a
		 * dwell slot. Stamping it on the failure paths instead would
		 * let a target that cannot fork right now (exiting, signal
		 * pending) demote itself out of contention for a condition
		 * that is transient and not its fault.
		 */
		current->ipcc_shadow_last_turn = ktime_get();
	}

deliver:
	if (atomic_cmpxchg(&req->state, 0, IPCC_REQ_DELIVERED) != 0) {
		/*
		 * Lost the race: a newer tick on this same core's slot already
		 * evicted us (ipcc_evict(), called from ipcc_classify_tick())
		 * before we got here, and won its own CAS - meaning it is
		 * relying on *us* to clean up, since it could not safely read
		 * ->shadow/->error at that point. Nobody is waiting on this
		 * request winding up in ipcc_queue at all anymore.
		 */
		if (req->shadow) {
			ipcc_kill_parked_shadow(req->shadow);
			put_task_struct(req->shadow);
		}
		put_task_struct(req->target);
		kfree(req);
		return;
	}

	wake_up(&ipcc_wait);
}

/*
 * Supersede whatever was previously occupying a core's slot. Called from
 * ipcc_classify_tick() (IRQs disabled) with @old already unlinked from
 * ipcc_queue - this only ever decides what happens to @old itself.
 *
 * The two sides of the ownership race mirror each other exactly: whichever
 * of ipcc_evict() and ipcc_shadow_fork_work() loses the 0 -> {EVICTED,
 * DELIVERED} CAS is the one with stale-or-absent data, so it does nothing
 * further; whichever wins learns from *that* that the other side already
 * has (or will have) valid ->shadow/->error, and is the one that must do
 * the actual teardown.
 */
static void ipcc_evict(struct ipcc_shadow_req *old)
{
	if (task_work_cancel(old->target, &old->work)) {
		/* Provably never ran at all - safe to free directly. */
		put_task_struct(old->target);
		kfree(old);
		return;
	}

	if (atomic_cmpxchg(&old->state, 0, IPCC_REQ_EVICTED) != 0) {
		/*
		 * Lost: the callback already reached IPCC_REQ_DELIVERED, so
		 * ->shadow/->error are valid now - tear down whatever it
		 * produced ourselves, since it will never be dwelt on.
		 */
		if (old->shadow) {
			ipcc_kill_parked_shadow(old->shadow);
			put_task_struct(old->shadow);
		}
		put_task_struct(old->target);
		kfree(old);
	}
	/*
	 * Won: the callback (whenever it runs, if ever) will see
	 * IPCC_REQ_EVICTED and clean up after itself - @old must not be
	 * touched again from here.
	 */
}

/**
 * ipcc_classify_submit - install @target (running on @cpu) as the pending
 * candidate for @cpu's slot, evicting whatever was there before
 *
 * Safe to call with IRQs disabled (uses GFP_ATOMIC) and never blocks: the
 * actual fork happens later, asynchronously, in @target's own context.
 *
 * Returns 0 if the request was installed, or a negative errno: -EINVAL if
 * @cpu is not an E-core under management, -ENOMEM, or whatever
 * task_work_add() itself returned (-ESRCH means @target is exiting).
 */
static int ipcc_classify_submit(struct task_struct *target, int cpu)
{
	struct ipcc_shadow_req *req, *old;
	unsigned long flags;
	int slot, ret = 0;
	ktime_t now;

	slot = ipcc_cpu_to_slot[cpu];
	if (slot < 0)
		return -EINVAL;

	now = ktime_get();

	spin_lock_irqsave(&ipcc_queue_lock, flags);
	old = ipcc_queue[slot];
	if (old) {
		if (atomic_read(&old->state) == IPCC_REQ_DELIVERED)
			ret = -EBUSY;
		else if (!ktime_before(target->ipcc_shadow_last_turn,
				      old->target->ipcc_shadow_last_turn) &&
			 ktime_before(now, ktime_add_ms(old->t_inject,
							IPCC_SLOT_STALE_MS)))
			ret = -EBUSY;
	}
	spin_unlock_irqrestore(&ipcc_queue_lock, flags);

	if (ret)
		return ret;

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	get_task_struct(target);
	req->target = target;
	req->t_inject = ktime_get();
	init_task_work(&req->work, ipcc_shadow_fork_work);

	ret = task_work_add(target, &req->work, TWA_RESUME);
	if (ret) {
		put_task_struct(target);
		kfree(req);
		return ret;
	}

	spin_lock_irqsave(&ipcc_queue_lock, flags);
	old = ipcc_queue[slot];
	ipcc_queue[slot] = req;
	spin_unlock_irqrestore(&ipcc_queue_lock, flags);

	/* Whatever was in the slot before is superseded. */
	if (old)
		ipcc_evict(old);

	if (atomic_read(&req->state) == IPCC_REQ_DELIVERED)
		wake_up(&ipcc_wait);

	return 0;
}

/*
 * Sweep the slots in round-robin order starting at ipcc_rr_cursor, taking
 * the first one whose fork has completed. Starting from the cursor (rather
 * than always from slot 0) is what gives every E-core a turn within one lap
 * regardless of how hot any other core's residents are; taking the first
 * *ready* one found, rather than strictly advancing one slot per call, is
 * what keeps the classifier core from idling when work is sitting ready
 * behind a slot that just is not there yet (see ipcc_classify_submit() -
 * an empty or still-forking slot is a normal, common state, not congestion).
 */
static struct ipcc_shadow_req *ipcc_queue_next(void)
{
	struct ipcc_shadow_req *req = NULL;
	unsigned long flags;
	int i, slot;

	spin_lock_irqsave(&ipcc_queue_lock, flags);
	for (i = 0; i < ipcc_num_ecores; i++) {
		slot = (ipcc_rr_cursor + i) % ipcc_num_ecores;

		if (!ipcc_queue[slot] ||
		    atomic_read(&ipcc_queue[slot]->state) != IPCC_REQ_DELIVERED)
			continue;

		req = ipcc_queue[slot];
		ipcc_queue[slot] = NULL;
		ipcc_rr_cursor = (slot + 1) % ipcc_num_ecores;
		break;
	}
	spin_unlock_irqrestore(&ipcc_queue_lock, flags);

	return req;
}

static bool ipcc_queue_has_ready(void)
{
	unsigned long flags;
	bool ready = false;
	int i;

	spin_lock_irqsave(&ipcc_queue_lock, flags);
	for (i = 0; i < ipcc_num_ecores; i++) {
		if (ipcc_queue[i] && atomic_read(&ipcc_queue[i]->state) == IPCC_REQ_DELIVERED) {
			ready = true;
			break;
		}
	}
	spin_unlock_irqrestore(&ipcc_queue_lock, flags);

	return ready;
}

/*
 * Fixed-point (1 - 2^-IPCC_WEIGHT_SHIFT)^k, in the same IPCC_WEIGHT_SCALE
 * fixed-point base the weight vector itself uses. Bounded by @k, which is
 * itself bounded (unsigned char, and in practice never exceeds roughly a
 * shadow's dwell length in ticks) - cheap even as a plain loop, since this
 * runs at most once per classify-dwell cycle.
 */
static unsigned long ipcc_decay_factor(unsigned int k)
{
	unsigned long factor = IPCC_WEIGHT_SCALE;
	unsigned int i;

	for (i = 0; i < k; i++)
		factor -= factor >> IPCC_WEIGHT_SHIFT;

	return factor;
}

/**
 * ipcc_blend_class_weight - apply to @target the k EWMA steps it would have
 * taken had it been the one running on a P-core, where k is how many
 * confirmed readings @shadow actually gathered on its behalf
 *
 * The shadow's own ipcc_class_weight[] is deliberately *not* the target here.
 * It cannot be: shadow_copy_process() goes through sched_fork() ->
 * __sched_fork(), which zeroes the vector (kernel/sched/core.c), so a shadow
 * builds its vector up from nothing during its dwell rather than inheriting
 * @target's. Converging @target toward that vector is only sane once it has
 * had enough ticks to settle - and is actively wrong when it has not. At k=1
 * the shadow's vector still sits at IPCC_WEIGHT_SCALE >> IPCC_WEIGHT_SHIFT
 * for the observed class and zero elsewhere, so converging toward it would
 * *lower* @target's confidence in the very class the reading just confirmed,
 * while decaying every other class normally.
 *
 * So the evidence is applied directly instead. One confirmed reading of class
 * c is, by definition, one call to update_ipcc_class_weights(target, c) - and
 * k of them in a row have a closed form, since each class index moves
 * geometrically toward a fixed endpoint:
 *
 *   class c:   w += (SCALE - w) >> SHIFT   =>  SCALE - w_k = (SCALE - w0)*d^k
 *   others:    w -= w >> SHIFT             =>          w_k = w0 * d^k
 *
 * with d = 1 - 2^-SHIFT. Both are the same expression - converge w toward a
 * goal by a factor of d^k - with the goal being IPCC_WEIGHT_SCALE for the
 * observed class and 0 for every other. That makes @shadow the carrier of
 * exactly two things, ->ipcc and ->ipcc_confirm_count; its own weight vector
 * is written by the tick path but read by nobody (not worth special-casing
 * update_ipcc_class_weights(), which is hot for every task on the system, to
 * save a few stores on an isolated dedicated core).
 *
 * k scales the sample honestly by how long the hardware actually watched:
 * a full ~10ms dwell at HZ=1000 yields up to ~9 confirmed ticks, moving
 * @target roughly 44% of the way to the observed class ((1 - 2^-4)^9), while a
 * shadow that died on its first syscall carries k=1 and moves it by one tick's
 * worth - the same as if @target itself had been observed for that one sliver.
 * At k=0 (nothing was ever confirmed) this is the identity, which is why
 * callers gate on @shadow having committed a class at all.
 *
 * @target may be read concurrently by the load balancer from another cpu
 * (ipcc_weighted_score(), kernel/sched/sched.h) and written concurrently by
 * @target's own sched_tick() if it is running elsewhere right now;
 * READ_ONCE/WRITE_ONCE match that side's discipline. @shadow is still
 * technically alive at this point (not yet SIGKILLed) and pinned to the
 * classifier cpu, so in principle still tickable - READ_ONCE on it too.
 */
static void ipcc_blend_class_weight(struct task_struct *target, struct task_struct *shadow)
{
	unsigned long factor = ipcc_decay_factor(READ_ONCE(shadow->ipcc_confirm_count));
	unsigned short class = READ_ONCE(shadow->ipcc);
	int i;

	for (i = 0; i < NR_IPC_CLASSES; i++) {
		unsigned short cur = READ_ONCE(target->ipcc_class_weight[i]);
		unsigned short goal = (i == class) ? IPCC_WEIGHT_SCALE : 0;
		unsigned short new_w;

		if (goal >= cur)
			new_w = goal - (unsigned short)(((unsigned long)(goal - cur) * factor) / IPCC_WEIGHT_SCALE);
		else
			new_w = goal + (unsigned short)(((unsigned long)(cur - goal) * factor) / IPCC_WEIGHT_SCALE);

		WRITE_ONCE(target->ipcc_class_weight[i], new_w);
	}
}

/**
 * ipcc_classify_dwell - give @req's shadow its turn on the classifier core
 *
 * Only ever called from ipcc_reaper's single-threaded loop, one @req at a
 * time - this, not a mutex, is what guarantees only one shadow is ever
 * actually placed on the classifier cpu, no matter how many other requests
 * are concurrently forked and sitting in ipcc_queue.
 *
 * Consumes (frees) @req and drops its target/shadow references before
 * returning.
 */
static void ipcc_classify_dwell(struct ipcc_shadow_req *req)
{
	struct task_struct *target = req->target;
	struct task_struct *shadow = req->shadow;
	ktime_t t_prepin, t_predwell, t_postdwell;
	const char *outcome;
	long wait_ret;
	bool applied;

	if (req->error) {
		/*
		 * Transient failure (signal pending, target already exiting).
		 * Nothing to unwind: there is no bookkeeping of our own to
		 * restore - @target simply stays at whatever class it had and
		 * will be offered again the next time it ticks on an E-core.
		 */
		// trace_printk("ipcc-classifier: forkfail: target=%d ret=%d\n",
		//	     target->pid, req->error);
		put_task_struct(target);
		kfree(req);
		return;
	}

	/*
	 * Only now does the shadow start existing as a runnable thing at all.
	 * It was forked - possibly long ago - already bound to the classifier
	 * cpu but parked in TASK_NEW, on no runqueue, costing nothing; see
	 * shadow_kernel_clone()'s doc comment in kernel/fork.c. This is not a
	 * migration: with nr_cpus_allowed == 1, select_task_rq() inside
	 * wake_up_new_task() just takes cpumask_any(p->cpus_ptr) and the task
	 * materialises directly on the classifier cpu.
	 *
	 * Because only this single-threaded loop ever wakes a shadow, and only
	 * one at a time, "exactly one shadow runnable on the classifier core"
	 * holds by construction rather than by timing - which is what ITD needs,
	 * its classification being per-core.
	 */
	t_prepin = ktime_get();

	wake_up_new_task(shadow);

	t_predwell = ktime_get();

	/*
	 * Let the hardware watch the shadow - but only for as long as there is
	 * something to watch. IPCC_SHADOW_DWELL_MS is a budget, not a duration:
	 * a shadow that dies on its first syscall (which for an event-loop
	 * target happens in microseconds) publishes ipcc_shadow_died_at and
	 * wakes us from ipcc_shadow_syscall_denied(), and the classifier core
	 * is freed for the next candidate immediately instead of sleeping out
	 * the remaining tens of milliseconds against a task that no longer
	 * exists. Compute-bound targets - the ones that actually classify -
	 * never make a syscall and still use the whole budget.
	 *
	 * The predicate has to be ipcc_shadow_died_at rather than anything in
	 * the exit path: the wake happens *before* do_exit(), so exit_state is
	 * still 0 at that moment and waiting on it would just put us straight
	 * back to sleep. Paired with the smp_store_release() on the other side
	 * so the committed class is visible along with the timestamp.
	 *
	 * Interruptible so the reaper sleeps in S rather than D and stays out
	 * of the load average. All three ways out mean the same thing here -
	 * stop observing - so none of them changes what happens next; the
	 * return value is kept only to label the trace line, since which of
	 * them happened is exactly what says whether IPCC_SHADOW_DWELL_MS is
	 * set sensibly.
	 */
	wait_ret = wait_event_interruptible_timeout(ipcc_dwell_wait,
						    smp_load_acquire(&shadow->ipcc_shadow_died_at) ||
						    smp_load_acquire(&shadow->ipcc_shadow_confirmed_at),
						    msecs_to_jiffies(IPCC_SHADOW_DWELL_MS));

	t_postdwell = ktime_get();

	/*
	 * Lazy, single-shot write: nothing is copied to target until the
	 * shadow is done (died on its own or reaped below), and then
	 * everything - ipcc and the weight blend - lands in one go. Applied
	 * whenever the shadow committed a class; ipcc == 0 means the
	 * debouncer never settled, and copying that would erase whatever the
	 * target already knew about itself.
	 *
	 * ipcc_prev/ipcc_stable_count are never copied: those are debounce
	 * state, meaningful only relative to a continuous stream of readings
	 * on one task. Leaving the target's own state untouched means that
	 * once it does reach a P-core its debouncer starts clean and can
	 * correct this estimate, which is exactly the desired precedence.
	 *
	 * ipcc_class_weight[] *is* updated alongside ipcc, under the same
	 * guard: update_ipcc_class_weights() (sched_ipcc.c) only ever nudges
	 * it from the same commit branch that sets ipcc, so it is populated
	 * iff ipcc is - there is nothing to gain from a separate condition.
	 * The shadow needs no extra instrumentation for this: it is an
	 * ordinary task from the scheduler's point of view, so it accumulates
	 * its own weight vector for free while it dwells on the classifier
	 * cpu, through the exact same intel_update_ipcc()/
	 * debounce_and_update_class() path a real P-core-resident task would.
	 *
	 * Blended in with the same EWMA the vector already uses (see
	 * ipcc_blend_class_weight() below), not overwritten outright: an
	 * outright copy would erase any real P-core history @target had
	 * before it ever migrated to an E-core, on every single cycle. The
	 * shadow's sample is weighed by how many confirmed ticks it actually
	 * gathered, exactly as if @target had produced that many real HFI
	 * ticks itself.
	 */
	applied = shadow->ipcc;
	if (applied) {
		WRITE_ONCE(target->ipcc, shadow->ipcc);
		ipcc_blend_class_weight(target, shadow);
	}

	/*
	 * One line per cycle, deliberately - this used to be four (inject
	 * breakdown, pin cost, copy-back decision, dwell), which at ~20 cycles
	 * a second was enough to matter. Together with the per-tick ipcc-raw
	 * tracing that used to live in intel_update_ipcc(), kernel logging
	 * reached ~1500 lines/s under load and saturated journald outright:
	 * systemd-journal-flush timed out after 90s and journald then missed
	 * its own 3-minute watchdog, which is what a hung boot-time start job
	 * turned out to be. Keep every field, spend one line.
	 *
	 * inject/cb: task_work round trip until the target ran our callback.
	 * fork: cost of shadow_kernel_clone() itself, in the target's context.
	 * wake: cost of wake_up_new_task() starting the parked shadow. No
	 * migration is involved (it was bound to this cpu at fork time), so
	 * this should stay near zero; a large value means the reaper itself
	 * was delayed getting onto the cpu.
	 * dwell: how long the shadow was actually observed. Ends early when the
	 * shadow dies on its own, so a value well under IPCC_SHADOW_DWELL_MS is
	 * the mechanism working, not a problem.
	 * life: wall-clock time the shadow spent running, from the wake that
	 * started it to its syscall-death; -1 means it never made a syscall and
	 * was still alive (killed by us) when the dwell budget ran out.
	 * Measured from t_predwell, not from the fork: a shadow is parked and
	 * unrunnable between the two, so counting that would report queue
	 * latency as if it were runtime.
	 * applied: whether the class was actually copied back (it is not, if
	 * the shadow never settled on one, or the target has since reached a
	 * P-core and can measure itself for real).
	 * k: confirmed readings the shadow gathered, which is how far
	 * ipcc_blend_class_weight() moves the target (0 = no evidence at all,
	 * ~9 = a full dwell at the current budget). w: the target's weight for the
	 * committed class, out of IPCC_WEIGHT_SCALE, or -1 if the class is out
	 * of range (see below). These two exist because applied=1 alone says
	 * nothing about whether the copy-back moved anything - it silently did
	 * not, for every short-lived shadow, until _final() started counting
	 * its reading as a tick.
	 *
	 * The line is tagged by how the dwell ended, because the two endings
	 * classify through completely different paths and mixing them hides
	 * which one is actually working:
	 *
	 *   syscall - the shadow hit its trap and died on its own. Its class
	 *             came from intel_classify_ipcc_final(), an unfiltered
	 *             single reading taken in the shadow's own context, which
	 *             is the only context where the MSR read means anything.
	 *             Expect k=1, small w, dwell well under the budget.
	 *   timeout - the shadow never made a syscall and outlived the budget;
	 *             we killed it. Its class can only have come from the
	 *             tick-driven debouncer, which needs CLASS_DEBOUNCER_SKIPS
	 *             consecutive valid readings. Expect dwell ~= the budget,
	 *             and either a large k or applied=0.
	 *   stopped - the wait was interrupted (kthread_stop). Not a sample.
	 *
	 * Grep them apart to size IPCC_SHADOW_DWELL_MS: a timeout line with
	 * k=0 means the budget expired before the hardware ever produced a
	 * valid reading, so shortening it would only produce more of the same.
	 */
	{
		ktime_t died_at = READ_ONCE(shadow->ipcc_shadow_died_at);
		long long life_us = died_at ?
			ktime_to_us(ktime_sub(died_at, t_predwell)) : -1;
		unsigned short class = shadow->ipcc;
		/*
		 * ->ipcc is not bounded by NR_IPC_CLASSES: it is classid + 1
		 * straight from MSR_IA32_HW_FEEDBACK_CHAR, whose classid field
		 * is 8 bits wide, and classification_is_accurate() waves
		 * through *any* value once the SMT siblings are idle - which,
		 * with the nosmt boot this design assumes, is always. Real ITD
		 * on these parts only reports 0-3, but nothing enforces it, so
		 * indexing by it needs the same guard the two sites in fair.c
		 * already use. The weight update paths do not need one: both
		 * update_ipcc_class_weights() and ipcc_blend_class_weight()
		 * compare the class against the loop index rather than
		 * indexing by it, so an out-of-range class simply decays every
		 * weight instead of boosting one.
		 */
		int w = (applied && class < NR_IPC_CLASSES) ?
			READ_ONCE(target->ipcc_class_weight[class]) : -1;

		if (died_at)
			outcome = "syscall";
		else if (READ_ONCE(shadow->ipcc_shadow_confirmed_at))
			outcome = "confirmed";
		else if (wait_ret < 0)
			outcome = "stopped";
		else
			outcome = "timeout";

		trace_printk("ipcc-classifier: %s: target=%d shadow=%d inject_cb=%lldus fork=%lldus wake=%lldus dwell=%lldus life=%lldus ipcc=%u applied=%d k=%u w=%d\n",
			     outcome, target->pid, shadow->pid,
			     ktime_to_us(ktime_sub(req->t_callback, req->t_inject)),
			     ktime_to_us(ktime_sub(req->t_forked, req->t_callback)),
			     ktime_to_us(ktime_sub(t_predwell, t_prepin)),
			     ktime_to_us(ktime_sub(t_postdwell, t_predwell)),
			     life_us, class, applied,
			     shadow->ipcc_confirm_count, w);
	}

	/*
	 * Tear the shadow down. Plain send_sig(), not ipcc_kill_parked_shadow():
	 * we woke this one ourselves above, so it is a normal running (or
	 * already exited) task and SIGKILL reaches it the usual way - and
	 * wake_up_new_task() must not be called on it a second time. Reaping is
	 * automatic: ipcc_reaper ignores SIGCHLD, so do_notify_parent()
	 * autoreaps it via exit_notify()/release_task().
	 */
	send_sig(SIGKILL, shadow, 1);
	put_task_struct(shadow);
	put_task_struct(target);
	kfree(req);
}

/*
 * Pick the P-core to classify on: never cpu 0, and never an E-core (which
 * cannot classify at all).
 */
static int __init ipcc_pick_classifier_cpu(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		if (ipcc_cpu_is_atom(cpu))
			continue;

		return cpu;
	}

	return -1;
}

/*
 * Build the per-E-core slot table: ipcc_queue/ipcc_ecore_cpus sized to the
 * number of E-cores actually present, plus the nr_cpu_ids-sized reverse
 * lookup ipcc_classify_submit() and ipcc_classify_tick() use to turn a raw
 * cpu number into a slot index in O(1). Must run before the reaper starts.
 */
static int __init ipcc_setup_ecore_slots(void)
{
	int cpu, i;

	ipcc_num_ecores = 0;
	for_each_online_cpu(cpu)
		if (ipcc_cpu_is_atom(cpu))
			ipcc_num_ecores++;

	if (!ipcc_num_ecores)
		return -ENODEV;

	ipcc_queue = kcalloc(ipcc_num_ecores, sizeof(*ipcc_queue), GFP_KERNEL);
	ipcc_ecore_cpus = kcalloc(ipcc_num_ecores, sizeof(*ipcc_ecore_cpus), GFP_KERNEL);
	ipcc_cpu_to_slot = kmalloc_array(nr_cpu_ids, sizeof(*ipcc_cpu_to_slot), GFP_KERNEL);
	if (!ipcc_queue || !ipcc_ecore_cpus || !ipcc_cpu_to_slot)
		return -ENOMEM;

	for (i = 0; i < nr_cpu_ids; i++)
		ipcc_cpu_to_slot[i] = -1;

	i = 0;
	for_each_online_cpu(cpu) {
		if (!ipcc_cpu_is_atom(cpu))
			continue;
		ipcc_ecore_cpus[i] = cpu;
		ipcc_cpu_to_slot[cpu] = i;
		i++;
	}

	return 0;
}

/*
 * Take the classifier cpu out of every scheduling domain, so the load
 * balancer never places, pulls or pushes real work onto it. Only tasks that
 * are explicitly pinned there - our shadows - can run on it.
 *
 * HK_TYPE_DOMAIN is the mask build_sched_domains() filters against, and
 * housekeeping_update() is the same entry point cpuset isolated partitions
 * use; it just does not rebuild the domains itself, hence the second call.
 */
static int __init ipcc_isolate_classifier_cpu(void)
{
	cpumask_var_t isol;
	int ret;

	if (!alloc_cpumask_var(&isol, GFP_KERNEL))
		return -ENOMEM;

	cpumask_clear(isol);
	cpumask_set_cpu(ipcc_classifier_cpu, isol);

	cpus_read_lock();
	ret = housekeeping_update(isol);
	cpus_read_unlock();

	free_cpumask_var(isol);

	if (ret)
		return ret;

	/* Takes cpus_read_lock() itself, so it must not be nested above. */
	rebuild_sched_domains();

	return 0;
}

/*
 * Clamp the classifier cpu to its lowest frequency: only the classification
 * itself matters there, never throughput, and a core running flat out for no
 * reason is pure waste of power and thermal headroom.
 *
 * FREQ_QOS_MAX (a ceiling pinned at the floor) rather than FREQ_QOS_MIN is
 * what makes this durable against the governor - the same lever thermal
 * throttling uses (see drivers/thermal/cpufreq_cooling.c).
 */
static int __init ipcc_pin_min_freq(void)
{
	struct cpufreq_policy *policy;
	int ret;

	policy = cpufreq_cpu_get(ipcc_classifier_cpu);
	if (!policy)
		return -ENODEV;

	ret = freq_qos_add_request(&policy->constraints, &ipcc_freq_req,
				   FREQ_QOS_MAX, policy->cpuinfo.min_freq);

	cpufreq_cpu_put(policy);

	return ret < 0 ? ret : 0;
}

/*
 * Candidate discovery is event-driven, not polled: ipcc_classify_tick()
 * (called from intel_update_ipcc() for whatever task is genuinely rq->curr
 * on an E-core at this user tick) is the only source of candidates. Reacting
 * to curr needs no guess: the task is provably running right now, so it is
 * provably about to reach resume_user_mode_work() on its way back to
 * userspace, which is exactly what ipcc_classify_submit() needs. It also
 * sidesteps cross-cgroup starvation entirely (an earlier, discarded design
 * walked CFS's own rb-tree to pick a candidate) - there is no tree walk, so
 * a heavily loaded sibling cgroup can never make its tasks structurally
 * unreachable. (A *different* kind of starvation - hot resident tasks
 * crowding out new arrivals for the classifier's attention - showed up
 * later and is what the per-E-core slot design above ipcc_evict() exists
 * to fix; see that comment.)
 */

/**
 * ipcc_classify_tick - consider @curr, genuinely running on E-core @cpu, as
 * the next classification candidate
 *
 * Called from intel_update_ipcc() with IRQs disabled, no locks held. Must
 * never sleep or block - see ipcc_classify_submit().
 *
 * Deliberately does not skip tasks that already have a class: on a real
 * P-core, intel_update_ipcc() reclassifies whatever is curr on *every*
 * user tick, unconditionally, forever - a shadow-classified E-core task
 * should be offered no less often. There is no admission gate here at all
 * on purpose, to keep the classifier as busy as it can possibly be;
 * ipcc_classify_submit() always installs @curr into @cpu's slot, evicting
 * whatever was there, which is what keeps a single hot task from doing
 * anything worse than repeatedly overwriting its own core's one slot.
 */
void ipcc_classify_tick(struct task_struct *curr, int cpu)
{
	if (!ipcc_classify_eligible(curr))
		return;

	ipcc_classify_submit(curr, cpu);
}

static int ipcc_reaper_fn(void *unused)
{
	/*
	 * The whole reap-without-disturbing-the-target design rests on this:
	 * with SIGCHLD ignored here, do_notify_parent() autoreaps every shadow
	 * parented to us. kthreads inherit an all-SIG_IGN table from kthreadd
	 * already, but that is an implementation detail of kthreadd - make the
	 * contract explicit rather than inherited.
	 */
	disallow_signal(SIGCHLD);

	// trace_printk("ipcc-classifier: reaper ready (classifier cpu %d)\n", ipcc_classifier_cpu);

	while (!kthread_should_stop()) {
		struct ipcc_shadow_req *req;

		wait_event_interruptible(ipcc_wait, kthread_should_stop() ||
					  ipcc_queue_has_ready());

		if (kthread_should_stop())
			break;

		req = ipcc_queue_next();
		if (!req)
			continue;

		ipcc_classify_dwell(req);
	}

	return 0;
}

/*
 * Debugfs trigger: write a pid to submit for classification. Goes through
 * the same queue as the live tick-triggered path, so - unlike before this
 * queue existed - this no longer classifies synchronously; the result shows
 * up in the ipcc-profile/copy-back log lines shortly after.
 */
static ssize_t ipcc_classify_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct task_struct *target;
	int ret, pid;

	ret = kstrtoint_from_user(ubuf, count, 0, &pid);
	if (ret)
		return ret;

	rcu_read_lock();
	target = find_task_by_vpid(pid);
	if (target)
		get_task_struct(target);
	rcu_read_unlock();

	if (!target)
		return -ESRCH;

	if (!ipcc_classify_eligible(target)) {
		put_task_struct(target);
		return -EINVAL;
	}

	/* -EINVAL from here means @pid is not on an E-core under management right now. */
	ret = ipcc_classify_submit(target, task_cpu(target));
	put_task_struct(target);	/* ipcc_classify_submit() took its own ref */

	return ret ? ret : count;
}

static const struct file_operations ipcc_classify_fops = {
	.owner	= THIS_MODULE,
	.write	= ipcc_classify_write,
	.llseek	= noop_llseek,
};

static int __init ipcc_classifier_init(void)
{
	struct task_struct *t;
	int ret;

	if (!cpu_feature_enabled(X86_FEATURE_ITD)) {
		// trace_printk("ipcc-classifier: Intel Thread Director not available, not starting\n");
		return 0;
	}

	ipcc_classifier_cpu = ipcc_pick_classifier_cpu();
	if (ipcc_classifier_cpu < 0) {
		pr_warn("no usable P-core found, not starting\n");
		return 0;
	}

	/*
	 * ITD classification is per-core, not per-thread, so an SMT sibling
	 * running unrelated work on the classifier core corrupts the reading.
	 * The design assumes SMT is off.
	 */
	if (cpumask_weight(topology_sibling_cpumask(ipcc_classifier_cpu)) > 1)
		pr_warn("SMT is active on cpu %d; classifications will be noisy (boot with nosmt)\n",
			ipcc_classifier_cpu);

	ret = ipcc_isolate_classifier_cpu();
	if (ret) {
		pr_err("failed to isolate cpu %d: %d\n", ipcc_classifier_cpu, ret);
		return ret;
	}

	ret = ipcc_setup_ecore_slots();
	if (ret) {
		pr_err("failed to set up E-core slots: %d\n", ret);
		return ret;
	}
	{
		cpumask_var_t ecores;
		int i;

		if (alloc_cpumask_var(&ecores, GFP_KERNEL)) {
			cpumask_clear(ecores);
			for (i = 0; i < ipcc_num_ecores; i++)
				cpumask_set_cpu(ipcc_ecore_cpus[i], ecores);
			// trace_printk("ipcc-classifier: managing %d E-core slot(s): %*pbl\n",
			//	     ipcc_num_ecores, cpumask_pr_args(ecores));
			free_cpumask_var(ecores);
		}
	}

	/*
	 * Not fatal: without the pin the core simply classifies at whatever
	 * frequency the governor picks, which costs power but not correctness.
	 * cpufreq may also just not be up yet at this point.
	 */
	ret = ipcc_pin_min_freq();
	if (ret)
		pr_warn("could not pin cpu %d to minimum frequency: %d\n",
			ipcc_classifier_cpu, ret);

	t = kthread_run(ipcc_reaper_fn, NULL, "ipcc-reaper");
	if (IS_ERR(t)) {
		pr_err("failed to start reaper: %ld\n", PTR_ERR(t));
		return PTR_ERR(t);
	}
	ipcc_reaper = t;

	debugfs_create_file("ipcc_classify", 0200, NULL, NULL,
			    &ipcc_classify_fops);

	return 0;
}
late_initcall(ipcc_classifier_init);
