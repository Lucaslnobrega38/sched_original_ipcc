// SPDX-License-Identifier: GPL-2.0-only
/*
 * Active IPC-class classification of E-core-resident tasks.
 *
 * Forks a throwaway COW shadow of an E-core-resident target, pins it to a
 * dedicated P-core so Intel Thread Director can classify it, copies the
 * class back, destroys the shadow. See context.md for the full design.
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

/* cpu_type's core-type bits are 31:24; every E/P check goes through here. */
static inline bool ipcc_cpu_is_atom(int cpu)
{
	return (cpu_data(cpu).topo.cpu_type >> 24) == INTEL_CPU_TYPE_ATOM;
}

/* Dwell budget, not a duration - ends early if the shadow dies first. */
#define IPCC_SHADOW_DWELL_MS		10

/* Seniority veto ceiling for a pending (not yet forked) slot occupant. */
#define IPCC_SLOT_STALE_MS		2000

/* Floor on lag since a target's last turn before it is offered another. */
#define IPCC_MIN_LAG_MS			1

static int ipcc_classifier_cpu = -1;
static struct task_struct *ipcc_reaper;
static struct freq_qos_request ipcc_freq_req;

/* Diagnostic only: lets sched_ipcc.c log classifier-cpu activity specially. */
int ipcc_get_classifier_cpu(void)
{
	return ipcc_classifier_cpu;
}

/*
 * One heap-allocated request per fork attempt; the fork itself has to run in
 * the target's own context, so it is injected as a task_work and the result
 * comes back through this struct.
 */
struct ipcc_shadow_req {
	struct callback_head	work;
	struct task_struct	*target;	/* ref held; who to copy the class back to */
	struct task_struct	*shadow;	/* ref held once forked; NULL until delivered */
	int			error;		/* set instead of ->shadow on failure */

	/* Whoever wins the 0 -> {EVICTED,DELIVERED} CAS owns cleanup - see
	 * ipcc_shadow_fork_work() and ipcc_evict().
	 */
	atomic_t		state;
#define IPCC_REQ_EVICTED	1
#define IPCC_REQ_DELIVERED	2

	/* Profiling timestamps; t_inject at submission, the rest inside
	 * ipcc_shadow_fork_work(), which runs in the target's context.
	 */
	ktime_t			t_inject;
	ktime_t			t_callback;
	ktime_t			t_forked;
};

/* One slot per managed E-core; a tick always replaces whatever was in its
 * own core's slot (ipcc_evict()) rather than being turned away. ipcc_reaper
 * visits slots round-robin (ipcc_queue_next()), one dwell at a time - that,
 * not a mutex, is what keeps only one shadow ever on the classifier core.
 */
static struct ipcc_shadow_req **ipcc_queue;	/* ipcc_num_ecores entries */
static int *ipcc_ecore_cpus;			/* ipcc_num_ecores entries: the actual cpu numbers */
static int *ipcc_cpu_to_slot;			/* nr_cpu_ids entries: reverse lookup, -1 if not managed */
static int ipcc_num_ecores;
static unsigned int ipcc_rr_cursor;		/* only ever touched by ipcc_reaper */
static DEFINE_SPINLOCK(ipcc_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(ipcc_wait);

/*
 * Single-threaded non-kernel tasks with a real mm only - CLONE_VM would give
 * the shadow no COW isolation. Shadows exclude themselves too (load-bearing:
 * otherwise a shadow could shadow itself, recursively).
 */
static bool ipcc_classify_eligible(struct task_struct *p)
{
	enum ipcc_shadow_status status;

	if (!p->mm || (p->flags & PF_KTHREAD) || get_nr_threads(p) != 1 ||
	    test_task_syscall_work(p, IPCC_SHADOW))
		return false;

	/* SCHEDULED refused alongside DELIVERED - see context.md ("at most
	 * one shadow per target") for why SCHEDULED alone isn't enough.
	 */
	status = smp_load_acquire(&p->ipcc_shadow_status);
	if (status == IPCC_SHADOW_DELIVERED || status == IPCC_SHADOW_SCHEDULED)
		return false;

	if (ktime_before(ktime_get(),
			 ktime_add_ms(p->ipcc_shadow_last_turn, IPCC_MIN_LAG_MS)))
		return false;

	return true;
}

/* Woken by a shadow about to die on its own, so ipcc_classify_dwell() can
 * stop watching immediately instead of sleeping out the rest of its budget.
 */
static DECLARE_WAIT_QUEUE_HEAD(ipcc_dwell_wait);

/*
 * Kill a shadow that was never woken. TASK_NEW ignores a bare SIGKILL
 * (outside TASK_WAKEKILL), so signal first, then wake - it exits in
 * ret_from_fork without executing a userspace instruction. Only valid for a
 * shadow not yet woken; ipcc_classify_dwell() uses plain send_sig() instead.
 */
static void ipcc_kill_parked_shadow(struct task_struct *shadow)
{
	send_sig(SIGKILL, shadow, 1);
	wake_up_new_task(shadow);
}

/*
 * A shadow attempted a syscall - called from syscall_trace_enter() before
 * the syscall runs. Does not return.
 */
void __noreturn ipcc_shadow_syscall_denied(void)
{
	intel_classify_ipcc_final(current);

	smp_store_release(&current->ipcc_shadow_died_at, ktime_get());
	wake_up(&ipcc_dwell_wait);

	do_exit(SIGKILL);
}

/*
 * Called from intel_update_ipcc() the moment a shadow's own tick produces a
 * valid class. Guarded so only the first call stamps the timestamp.
 */
void ipcc_shadow_confirmed(struct task_struct *p)
{
	if (READ_ONCE(p->ipcc_shadow_confirmed_at))
		return;

	smp_store_release(&p->ipcc_shadow_confirmed_at, ktime_get());
	wake_up(&ipcc_dwell_wait);
}

/* Runs in the target's own context, from the exit-to-usermode task_work
 * loop: no locks held, IRQs enabled, sleepable - what copy_process() needs.
 */
static void ipcc_shadow_fork_work(struct callback_head *work)
{
	struct ipcc_shadow_req *req = container_of(work, struct ipcc_shadow_req, work);
	struct task_struct *shadow;

	req->t_callback = ktime_get();

	/* current may be exiting (do_exit()'s own leftover task_work run),
	 * not just returning to userspace normally - see context.md.
	 */
	if (current->flags & PF_EXITING) {
		req->error = -ESRCH;
		goto deliver;
	}

	shadow = shadow_kernel_clone(ipcc_reaper, ipcc_classifier_cpu);

	req->t_forked = ktime_get();

	if (IS_ERR(shadow)) {
		req->error = PTR_ERR(shadow);
	} else {
		req->shadow = shadow;
		current->ipcc_shadow_last_turn = ktime_get();
	}

deliver:
	if (atomic_cmpxchg(&req->state, 0, IPCC_REQ_DELIVERED) != 0) {
		/* Lost: a newer tick already evicted us and owns cleanup of
		 * ->ipcc_shadow_status; just tear down what we produced.
		 */
		if (req->shadow) {
			ipcc_kill_parked_shadow(req->shadow);
			put_task_struct(req->shadow);
		}
		put_task_struct(req->target);
		kfree(req);
		return;
	}

	if (req->shadow)
		smp_store_release(&current->ipcc_shadow_status, IPCC_SHADOW_DELIVERED);

	wake_up(&ipcc_wait);
}

/*
 * Supersede whatever was in a core's slot. Called from ipcc_classify_tick()
 * with @old already unlinked from ipcc_queue. Mirrors ipcc_shadow_fork_work()'s
 * CAS exactly: the loser has stale-or-absent data and does nothing further;
 * the winner tears down what the other side has (or will have) produced.
 */
static void ipcc_evict(struct ipcc_shadow_req *old)
{
	if (task_work_cancel(old->target, &old->work)) {
		WRITE_ONCE(old->target->ipcc_shadow_status, IPCC_SHADOW_EVICTED);
		put_task_struct(old->target);
		kfree(old);
		return;
	}

	if (atomic_cmpxchg(&old->state, 0, IPCC_REQ_EVICTED) != 0) {
		if (old->shadow) {
			ipcc_kill_parked_shadow(old->shadow);
			put_task_struct(old->shadow);
		}
		WRITE_ONCE(old->target->ipcc_shadow_status, IPCC_SHADOW_EVICTED);
		put_task_struct(old->target);
		kfree(old);
		return;
	}
	WRITE_ONCE(old->target->ipcc_shadow_status, IPCC_SHADOW_EVICTED);
}

/*
 * Install @target as the pending candidate for @cpu's slot, evicting
 * whatever was there. Safe with IRQs disabled, never blocks - the fork
 * happens later, asynchronously, in @target's own context.
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
	/* Authoritative re-check: ipcc_classify_eligible()'s lockless
	 * pre-check on this field can be stale by now.
	 */
	if (!ret) {
		enum ipcc_shadow_status status = READ_ONCE(target->ipcc_shadow_status);

		if (status == IPCC_SHADOW_DELIVERED || status == IPCC_SHADOW_SCHEDULED)
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

	WRITE_ONCE(target->ipcc_shadow_status, IPCC_SHADOW_SCHEDULED);

	spin_lock_irqsave(&ipcc_queue_lock, flags);
	old = ipcc_queue[slot];
	ipcc_queue[slot] = req;
	spin_unlock_irqrestore(&ipcc_queue_lock, flags);

	if (old)
		ipcc_evict(old);

	if (atomic_read(&req->state) == IPCC_REQ_DELIVERED)
		wake_up(&ipcc_wait);

	return 0;
}

/* Round-robin sweep from ipcc_rr_cursor, taking the first slot whose fork
 * has completed - gives every E-core a turn per lap without idling behind
 * a slot that just hasn't finished forking yet.
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

/* (1 - 2^-IPCC_WEIGHT_SHIFT)^k in IPCC_WEIGHT_SCALE fixed point. */
static unsigned long ipcc_decay_factor(unsigned int k)
{
	unsigned long factor = IPCC_WEIGHT_SCALE;
	unsigned int i;

	for (i = 0; i < k; i++)
		factor -= factor >> IPCC_WEIGHT_SHIFT;

	return factor;
}

/*
 * Apply to @target the k EWMA steps it would have taken had it produced
 * @shadow's k confirmed readings itself. See context.md for the closed-form
 * derivation and why the shadow's own weight vector isn't used directly.
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

/*
 * Give @req's shadow its turn on the classifier core. Only ever called from
 * ipcc_reaper's single-threaded loop, one @req at a time. Consumes (frees)
 * @req and drops its target/shadow references before returning.
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
		//trace_printk("ipcc-classifier: forkfail: target=%d ret=%d\n",
		//	     target->pid, req->error);
		WRITE_ONCE(target->ipcc_shadow_status, IPCC_SHADOW_NONE);
		put_task_struct(target);
		kfree(req);
		return;
	}

	t_prepin = ktime_get();

	wake_up_new_task(shadow);

	t_predwell = ktime_get();

	wait_ret = wait_event_interruptible_timeout(ipcc_dwell_wait,
						    smp_load_acquire(&shadow->ipcc_shadow_died_at) ||
						    smp_load_acquire(&shadow->ipcc_shadow_confirmed_at),
						    msecs_to_jiffies(IPCC_SHADOW_DWELL_MS));

	t_postdwell = ktime_get();

	/* Lazy single-shot copy-back: nothing lands on target until here. */
	applied = shadow->ipcc;
	if (applied) {
		WRITE_ONCE(target->ipcc, shadow->ipcc);
		ipcc_blend_class_weight(target, shadow);
	}

	{
		ktime_t died_at = READ_ONCE(shadow->ipcc_shadow_died_at);
		long long life_us = died_at ?
			ktime_to_us(ktime_sub(died_at, t_predwell)) : -1;
		unsigned short class = shadow->ipcc;
		/* ->ipcc is classid+1 from an 8-bit hardware field, not bounded
		 * by NR_IPC_CLASSES - guard the array index.
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

		//trace_printk("ipcc-classifier: %s: target=%d shadow=%d inject_cb=%lldus fork=%lldus wake=%lldus dwell=%lldus life=%lldus ipcc=%u applied=%d k=%u w=%d\n",
		//	     outcome, target->pid, shadow->pid,
		//	     ktime_to_us(ktime_sub(req->t_callback, req->t_inject)),
		//	     ktime_to_us(ktime_sub(req->t_forked, req->t_callback)),
		//	     ktime_to_us(ktime_sub(t_predwell, t_prepin)),
		//	     ktime_to_us(ktime_sub(t_postdwell, t_predwell)),
		//	     life_us, class, applied,
		//	     shadow->ipcc_confirm_count, w);
	}

	send_sig(SIGKILL, shadow, 1);
	WRITE_ONCE(target->ipcc_shadow_status, IPCC_SHADOW_NONE);
	put_task_struct(shadow);
	put_task_struct(target);
	kfree(req);
}

/* Never cpu 0, never an E-core. Highest-numbered P-core, deliberately -
 * see context.md (kernel/sched/fair.c's swap range depends on it).
 */
static int __init ipcc_pick_classifier_cpu(void)
{
	int cpu, best = -1;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		if (ipcc_cpu_is_atom(cpu))
			continue;

		best = cpu;
	}

	return best;
}

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

/* Pulls the classifier cpu out of scheduling so only pinned shadows run there. */
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
 * Consider @curr, genuinely running on E-core @cpu, as the next candidate.
 * Called from intel_update_ipcc() with IRQs disabled; must never sleep.
 */
void ipcc_classify_tick(struct task_struct *curr, int cpu)
{
	if (!ipcc_classify_eligible(curr))
		return;

	ipcc_classify_submit(curr, cpu);
}

static int ipcc_reaper_fn(void *unused)
{
	disallow_signal(SIGCHLD);

	//trace_printk("ipcc-classifier: reaper ready (classifier cpu %d)\n", ipcc_classifier_cpu);

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

/* Debugfs trigger: write a pid to submit for classification. */
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

	ret = ipcc_classify_submit(target, task_cpu(target));
	put_task_struct(target);

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
		//trace_printk("ipcc-classifier: Intel Thread Director not available, not starting\n");
		return 0;
	}

	ipcc_classifier_cpu = ipcc_pick_classifier_cpu();
	if (ipcc_classifier_cpu < 0) {
		pr_warn("no usable P-core found, not starting\n");
		return 0;
	}

	/* ITD classifies per-core; an SMT sibling on the classifier core
	 * would corrupt readings. Design assumes SMT off (nosmt).
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
			//trace_printk("ipcc-classifier: managing %d E-core slot(s): %*pbl\n",
			//	     ipcc_num_ecores, cpumask_pr_args(ecores));
			free_cpumask_var(ecores);
		}
	}

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
