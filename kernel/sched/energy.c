/* vim: set noet ts=8 sw=8 sts=8 : */

#include <asm/msr.h>
#include <asm/processor.h>

#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/syscalls.h>
#include <linux/timekeeping.h>

#include <linux/sched.h>

#include "sched.h"
#include "cpuacct.h"


/***
 * Internal constants
 ***/

/* Energy task states. */
enum {
	ETASK_RUNNING = 0x1
};

/* Thread states. */
enum {
	/* Thread RQ-queued states. */
	THREAD_RQ_RUNNABLE = 0x1,
	THREAD_RQ_QUEUED = THREAD_RQ_RUNNABLE,

	/* Thread CPU-queued states. */
	THREAD_CPU_RUNNABLE = 0x2,
	THREAD_CPU_QUEUED = THREAD_CPU_RUNNABLE,

	/* Thread running states. */
	THREAD_RUNNING = 0x4
};

/* Reschedule states. */
enum {
	LOCAL_RESCHED = 0x1
};

/* The default scheduling slice for one thread. --> 10ms <-- */
static const u64 THREAD_SCHED_SLICE = 10000000ULL;

/* The MSR numbers of the different RAPL counters. */
enum {
	/* The different counters. */
	ENERGY_PKG = MSR_PKG_ENERGY_STATUS,
	ENERGY_DRAM = MSR_DRAM_ENERGY_STATUS,
	ENERGY_CORE = MSR_PP0_ENERGY_STATUS,
	ENERGY_GPU = MSR_PP1_ENERGY_STATUS,

	/* The unit for the energy counters. */
	ENERGY_UNIT = MSR_RAPL_POWER_UNIT
};

/* Offsets and masks for the RAPL counters. */
enum {
	/* The different counters. */
	MASK_PKG = 0xffffffff,		/* Bits 31-0 */
	OFFSET_PKG = 0,			/* No shift needed. */

	MASK_DRAM = MASK_PKG,
	OFFSET_DRAM = OFFSET_PKG,

	MASK_CORE = MASK_PKG,
	OFFSET_CORE = OFFSET_PKG,

	MASK_GPU = MASK_PKG,
	OFFSET_GPU = OFFSET_PKG,

	/* The unit for the energy counters. */
	MASK_UNIT = 0x1f00,		/* Bits 12-8 */
	OFFSET_UNIT = 8			/* Shift by 8 bits. */
};

static const int ITERATIONS_INTERVAL_LENGTH = 100;
static const int ITERATIONS_LOOP_ENERGY = 50;


/***
 * Internal data structure prototypes.
 ***/

struct energy_task;
struct global_rq;
struct rapl_counters;
struct rapl_info;


/***
 * Internal data structure definitions.
 ***/

/* The representation of a task which should be run which energy accounting
 * enabled. */
struct energy_task {
	/* Is it currently running. */
	int state;

	/* The task struct belonging to the real task. */
	struct task_struct* task;

	/* The energy domain where the task should run. */
	struct cpumask domain;

	/* All runnable threads. */
	struct list_head runnable;
	u32 nr_runnable;

	/* The link in the global runqueue. */
	struct list_head rq;

	/* Runtime statistics */
	u64 start_running;
};

/* The global runqueue for all task with their corresponding threads which
 * are managed by this scheduling class. */
struct global_rq {
	/* Lock for the global runqueue. */
	raw_spinlock_t lock;

	/* Is the scheduling class currently running. */
	int running;

	/* All energy tasks. */
	struct list_head tasks;
	u32 nr_tasks;

	/* The total number of runnable threads. */
	u32 nr_threads;

	/* Runtime statistics */
	u64 start_running;
	u64 stop_running;
};

/* The RAPL counter state. */
struct rapl_counters {
	/* The time at which the counters were last updated. */
	ktime_t last_update;

	/* The value of the package counter. */
	u32 package;

	/* The value of the dram counter. */
	u32 dram;

	/* The value of the core counter. */
	u32 core;

	/* The value of the gpu counter. */
	u32 gpu;
};

/* The RAPL subsystem state. */
struct rapl_info {
	/* How long is an average update interval. */
	u32 update_interval;

	/* What is the energy unit of the RAPL counters. */
	u32 unit;

	/* How much energy is spent during looping at each counter. */
	u32 loop_package;
	u32 loop_dram;
	u32 loop_core;
	u32 loop_gpu;
};

/***
 * Internal variables.
 ***/

static struct global_rq grq;

static struct rapl_counters grc;
static struct rapl_info gri;

/***
 * Internal function prototypes.
 ***/

/* Working with the global runqueue. */
static void init_grq(void);
static void lock_grq(void);
static void unlock_grq(void);

/* Working with the rapl counters. */
static void init_rapl_counters(struct rapl_counters*);
static u64 read_rapl_counters(struct rapl_counters*, bool);
static void copy_rapl_counters(struct rapl_counters*, struct rapl_counters*);

/* Working with the global rapl counters. */
static void init_grc(void);

/* Working with the global rapl info. */
static void init_gri(void);

/* Working with the energy runqueues. */
static void lock_local_rq(struct rq*);
static void unlock_local_rq(struct rq*);

/* Get the real task belonging to a linux task. */
static struct task_struct* find_task(struct task_struct*);

/* Init energy tasks. */
static void init_energy_task(struct energy_task*);

/* Energy tasks and the global runqueue. */
static void enqueue_energy_task(struct energy_task*);
static void dequeue_energy_task(struct energy_task*);

/* Find an energy task belonging to a linux task. */
static struct energy_task* find_energy_task(struct task_struct*);
static struct energy_task* create_energy_task(struct task_struct*);
static void free_energy_task(struct energy_task*);

/* Threads and energy tasks. */
static bool thread_on_rq_queued(struct task_struct*);
static bool thread_on_cpu_rq_queued(struct task_struct*);
static bool thread_cpu_running(struct task_struct*);

static void enqueue_runnable(struct rq*, struct energy_task*,
		struct task_struct*);
static void dequeue_runnable(struct energy_task*, struct task_struct*);

static void enqueue_running(struct rq*, struct task_struct*);
static void dequeue_running(struct task_struct*);

/* Determining the scheduling slices. */
static u64 sched_slice_class(void);
static u64 sched_slice_energy(struct energy_task*);
static u64 sched_slice_local(struct rq*);
static u64 sched_slice_other(void);

/* Should we perform a scheduling operation? */
static bool should_switch_to_energy(struct rq*);
static bool should_check_cpus(void);
static bool should_switch_from_energy(struct rq*);
static bool should_switch_in_energy(struct rq*);
static bool should_switch_local(struct rq*);

/* Should we redistribute an energy task again? */
static bool should_redistribute_energy(struct energy_task*,
		struct task_struct*);

/* Set runqueue bits to perform scheduling operations. */
static void resched_curr_local(struct rq*);
static bool need_resched_curr_local(struct rq*);
static void clear_resched_curr_local(struct rq*);

/* Update runtime statistics. */
static void update_energy_statistics(struct energy_task*);
static void update_local_statistics(struct rq*, struct task_struct*);

/* Schedule and remove energy tasks. */
static void set_energy_task(struct rq*, struct energy_task*);
static void set_local_task(struct rq*, struct task_struct*);

static void distribute_energy_task(struct rq*, struct energy_task*);
static void distribute_local_task(struct rq*, struct task_struct*);

static void redistribute_energy_task(struct rq*, struct energy_task*, bool);

static void move_local_task(struct task_struct*, unsigned int);

static void clear_energy_task(struct energy_task*);
static void clear_local_tasks(struct rq*);

static void put_energy_task(struct rq*, struct energy_task*);
static void put_local_task(struct rq*, struct task_struct*);

static struct energy_task* pick_next_energy_task(void);
static struct task_struct* pick_next_local_task(struct rq*);

static void acquire_cpus(struct cpumask*);
static void release_cpus(struct cpumask*);

static void switch_to_energy(struct rq*, struct energy_task*);
static void switch_from_energy(struct rq*, struct energy_task*);
static void switch_in_energy(struct rq*, struct energy_task*, struct energy_task*);

/* Initialize the energy domain. */
static void init_energy_domain(struct cpumask*, unsigned int);

/* Idle function. */
static int idle_thread_fn(void*);

/* Switch to and from the energy scheduling class. */
static int do_start_energy(pid_t pid);
static int do_stop_energy(pid_t pid);


/***
 * External function prototypes
 ***/

/* Functions needed for the scheduling class implementation. */
void enqueue_task_energy(struct rq*, struct task_struct*, int);
void dequeue_task_energy(struct rq*, struct task_struct*, int);

void yield_task_energy(struct rq*);
bool yield_to_task_energy(struct rq*, struct task_struct*, bool);

void check_preempt_curr_energy(struct rq*, struct task_struct*, int);

struct task_struct* pick_next_task_energy(struct rq*, struct task_struct*);

void put_prev_task_energy(struct rq*, struct task_struct*);

void set_curr_task_energy(struct rq*);

void task_tick_energy(struct rq*, struct task_struct*, int);
void task_fork_energy(struct task_struct*);
void task_dead_energy(struct task_struct*);

void switched_from_energy(struct rq*, struct task_struct*);
void switched_to_energy(struct rq*, struct task_struct*);
void prio_changed_energy(struct rq*, struct task_struct*, int);

unsigned int get_rr_interval_energy(struct rq*, struct task_struct*);

void update_curr_energy(struct rq*);

int select_task_rq_energy(struct task_struct*, int, int, int);
void migrate_task_rq_energy(struct task_struct*, int);

void task_waking_energy(struct task_struct*);
void task_woken_energy(struct rq*, struct task_struct*);

void set_cpus_allowed_energy(struct task_struct*, const struct cpumask*);

void rq_online_energy(struct rq*);
void rq_offline_energy(struct rq*);


/***
 * Internal functions which are part of others.
 ***/

/* Properly increment the number of running tasks on a CPU runqueue.
 *
 * Requires that the local runqueue lock is taken.
 *
 * @rq:		the runqueue where the number of running tasks should be incremented.
 */
static inline void __inc_nr_running(struct rq* rq) {
	if (!rq->en.blocked) {
		add_nr_running(rq, 1);
	}

	rq->en.nr_assigned++;
}

/* Properly decrement the number of running tasks on a CPU runqueue.
 *
 * Requires that the local runqueue lock is taken.
 *
 * @rq:		the runqueue where the number of running tasks should be decremented.
 */
static inline void __dec_nr_running(struct rq* rq) {
	if (!rq->en.blocked) {
		sub_nr_running(rq, 1);
	}

	rq->en.nr_assigned--;
}

static void __distribute_energy_task(struct energy_task* e_task) {
	struct task_struct* thread;
	int cpu;

	/* Distribute all runnable threads belonging to the current energy task
	 * on the available CPUs in the energy domain. */
	list_for_each_entry(thread, &(e_task->runnable), ee.rq) {
		if (thread_on_cpu_rq_queued(thread)) {
			/* This thread is already assigned to a CPU runqueue. No
			 * need to do this again. */
			continue;
		} else {
			/* Find the CPU where the thread can run and which has the lowest
			 * load. Start with the one where the thread is already assigned to. */
			struct rq* best_rq = task_rq(thread);
			int min_load = best_rq->en.nr_runnable;

			for_each_cpu_and(cpu, &(e_task->domain), &(thread->cpus_allowed)) {
				int load = cpu_rq(cpu)->en.nr_runnable;

				if (load < min_load) {
					min_load = load;
					best_rq = cpu_rq(cpu);
				}
			}

			distribute_local_task(best_rq, thread);
		}
	}

	/* Set on all runqueues that the current energy task is running. */
	for_each_cpu(cpu, &(e_task->domain)) {
		struct rq* c_rq = cpu_rq(cpu);
		set_energy_task(c_rq, e_task);

		resched_curr_local(c_rq);
	}

}

static inline void __acquire_cpu(struct rq* rq) {
	lock_local_rq(rq);

	rq->en.blocked = false;
	add_nr_running(rq, rq->en.nr_assigned);

	unlock_local_rq(rq);
}

static inline void __release_cpu(struct rq* rq) {
	lock_local_rq(rq);

	rq->en.blocked = true;
	sub_nr_running(rq, rq->en.nr_assigned);

	unlock_local_rq(rq);
}

static inline void __switch_from_energy(struct rq* rq) {
	release_cpus(&(rq->en.domain));

	grq.running = 0;
	grq.stop_running = rq_clock(rq);
}

static inline void __switch_to_energy(struct rq* rq) {
	grq.running = 1;
	grq.start_running = rq_clock(rq);

	acquire_cpus(&(rq->en.domain));
}

static inline u32 __diff_wa(u32 first, u32 second) {
	if (first < second) {
		return (U32_MAX - second) + first;
	} else {
		return first - second;
	}
}

static inline int __read_rapl_msr(u32* value, u32 msr_nr, u64 mask, u64 offset) {
	u64 val;
	int err;

	if (!value) {
		return -EINVAL;
	}

	if ((err = rdmsrl_safe(msr_nr, &val)) != 0) {
		return err;
	}

	*value = (val & mask) >> offset;
	return 0;
}

static inline int __read_rapl_msr_until_update(u32* value, u32 msr_nr,
		u64 mask, u64 offset, ktime_t* tick, u64* duration) {
	u32 start_val, tmp_val;
	ktime_t start_tick, end_tick;
	int err;

	start_tick = ktime_get();
	if ((err = __read_rapl_msr(&tmp_val, msr_nr, mask, offset)) != 0) goto fail;

	start_val = tmp_val;

	while (tmp_val == start_val) {
		if ((err = __read_rapl_msr(&tmp_val, msr_nr, mask, offset)) != 0) goto fail;
	}

	end_tick = ktime_get();

	if (tick) {
		*tick = end_tick;
	}

	if (value) {
		*value = tmp_val;
	}

	if (duration) {
		*duration = ktime_us_delta(end_tick, start_tick);
	}

	err = 0;
fail:
	return err;
}

static inline int __read_rapl_unit(u32* unit) {
	u32 val = 0;
	int err;

	if ((err = __read_rapl_msr(&val, ENERGY_UNIT, MASK_UNIT, OFFSET_UNIT)) != 0) {
		return err;
	}

	/* The corresponding unit is (1/2) ^ val Joules. Hence calculate (10 ^ 6) /
	 * (2 ^ val) and thereby get mirco Joules. */
	*unit = 1000000 / (1 << val);

	return 0;
}

static inline void __update_rapl_counter(u64* value, u32 consumption, u32 loop_duration,
		u32 avg_loop_consumption) {
	u32 loop_consumption = (avg_loop_consumption * loop_duration) / gri.update_interval;
	u32 final_consumption = loop_consumption > consumption ? 0 : consumption - loop_consumption;

	*value += final_consumption * gri.unit;
}


/***
 * Internal function definitions.
 ***/

/* Initialize the global runqueue. */
static void __init init_grq(void) {
	raw_spin_lock_init(&(grq.lock));

	INIT_LIST_HEAD(&(grq.tasks));
	grq.nr_tasks = 0;
	grq.nr_threads = 0;

	grq.stop_running = 0;
	grq.start_running = 0;
}

/* Lock the global runqueue. */
static void lock_grq(void) __acquires(grq.lock) {
	do_raw_spin_lock(&(grq.lock));
}

/* Unlock the global runqueue. */
static void unlock_grq(void) __releases(grq.lock) {
	do_raw_spin_unlock(&(grq.lock));
}

static void init_rapl_counters(struct rapl_counters* counters) {
	counters->last_update = ktime_set(0, 0);
	counters->package = 0;
	counters->dram = 0;
	counters->core = 0;
	counters->gpu = 0;
}

static u64 read_rapl_counters(struct rapl_counters* counters,
		bool wait_for_update) {
	u64 duration = 0;

	if (wait_for_update) {
		__read_rapl_msr_until_update(&(counters->package), ENERGY_PKG,
				MASK_PKG, OFFSET_PKG, &(counters->last_update),
				&duration);
	} else {
		__read_rapl_msr(&(counters->package), ENERGY_PKG, MASK_PKG,
				OFFSET_PKG);
		counters->last_update = ktime_get();
		duration = 0;
	}

	/* Read the other counter values. */
	__read_rapl_msr(&(counters->dram), ENERGY_DRAM, MASK_DRAM, OFFSET_DRAM);
	__read_rapl_msr(&(counters->core), ENERGY_CORE, MASK_CORE, OFFSET_CORE);
	__read_rapl_msr(&(counters->gpu), ENERGY_GPU, MASK_GPU, OFFSET_GPU);

	return duration;
}

static void copy_rapl_counters(struct rapl_counters* from, struct rapl_counters* to) {
	to->last_update = from->last_update;

	to->package = from->package;
	to->dram = from->dram;
	to->core = from->core;
	to->gpu = from->gpu;
}

static void __init init_grc(void) {
	init_rapl_counters(&grc);
	read_rapl_counters(&grc, true);
}

/* Initialize the global RAPL info. */
static void __init init_gri(void) {
	ktime_t time_begin, time_end;
	struct rapl_counters counters_begin, counters_end;
	int i;

	__read_rapl_msr_until_update(NULL, ENERGY_PKG, MASK_PKG, OFFSET_PKG,
			&time_begin, NULL);

	for (i = 0; i < ITERATIONS_INTERVAL_LENGTH; ++i) {
		__read_rapl_msr_until_update(NULL, ENERGY_PKG, MASK_PKG, OFFSET_PKG,
				&time_end, NULL);
	}

	gri.update_interval = ktime_us_delta(time_end, time_begin) /
		ITERATIONS_INTERVAL_LENGTH;


	read_rapl_counters(&counters_begin, true);

	for (i = 0; i < ITERATIONS_LOOP_ENERGY; ++i) {
		read_rapl_counters(&counters_end, true);
	}

	gri.loop_package = __diff_wa(counters_end.package, counters_begin.package) /
		ITERATIONS_LOOP_ENERGY;
	gri.loop_dram = __diff_wa(counters_end.dram, counters_begin.dram) /
		ITERATIONS_LOOP_ENERGY;
	gri.loop_core = __diff_wa(counters_end.core, counters_begin.core) /
		ITERATIONS_LOOP_ENERGY;
	gri.loop_gpu = __diff_wa(counters_end.gpu, counters_begin.gpu) /
		ITERATIONS_LOOP_ENERGY;


	__read_rapl_unit(&(gri.unit));
}

/* Lock the local energy rq embedded in the CPU runqueues.
 *
 * @rq:		the runqueue of the current CPU.
 */
static void lock_local_rq(struct rq* rq) __acquires(rq->en.lock) {
	do_raw_spin_lock(&(rq->en.lock));
}

/* Unlock the local energy rq embedded in the CPU runqueue.
 *
 * @rq:		the runqueue of the current CPU.
 */
static void unlock_local_rq(struct rq* rq) __releases(rq->en.lock) {
	do_raw_spin_unlock(&(rq->en.lock));
}

/* Find the real task corresponding to a task struct.
 *
 * @t:		the task struct for which the real task should be returned.
 *
 * @returns:	the real task corresponding to the given task struct.
 */
static struct task_struct* find_task(struct task_struct* t) {
	if (thread_group_leader(t)) {
		return t;
	} else {
		return t->group_leader;
	}
}

/* Initialize an energy task struct.
 *
 * @e_task:	the pointer to the energy task, which should be
 *		initialized.
 */
static void init_energy_task(struct energy_task* e_task) {
	e_task->state = 0;

	e_task->task = NULL;

	cpumask_clear(&(e_task->domain));

	INIT_LIST_HEAD(&(e_task->runnable));
	e_task->nr_runnable = 0;

	e_task->start_running = 0;
}

/* Enqueue an energy task in the global runqueue.
 *
 * Requires that the lock of the global runqueue is taken.
 *
 * @e_task:	the energy task which should be enqueued.
 */
static void enqueue_energy_task(struct energy_task* e_task) {
	list_add(&(e_task->rq), &(grq.tasks));
	grq.nr_tasks++;
}

/* Dequeue an energy task from the global runqueue.
 *
 * Requires that the lock of the global runqueue is taken.
 *
 * @e_task:	the energy task which should be dequeued.
 */
static void dequeue_energy_task(struct energy_task* e_task) {
	list_del(&(e_task->rq));
	grq.nr_tasks--;
}

/* Find the energy task struct corresponding to a linux task t.
 *
 * Requires that the lock of the global rq is taken.
 *
 * @t:		the task struct of the linux task for which the corresponding
 *		energy task struct should be returned.
 *
 * @returns:	the energy task struct corresponding to the given linux task.
 */
static struct energy_task* find_energy_task(struct task_struct* t) {
	struct task_struct* task = find_task(t);
	struct energy_task* e_task;

	list_for_each_entry(e_task, &(grq.tasks), rq) {
		if (e_task->task == task) {
			return e_task;
		}
	}

	return NULL;
}

/* Create an energy task corresponding to a linux task t.
 *
 * Requires that the lock of the global rq is taken.
 *
 * @t:		the task struct of the linux task for which the corresponding
 *		energy task should be created.
 */
static struct energy_task* create_energy_task(struct task_struct* t) {
	struct task_struct* task = find_task(t);
	struct energy_task* e_task;

	/* Create and initialize a new energy task. We are using  GFP_ATOMIC here
	 * as we are currently holding a lock. */
	e_task = kmalloc(sizeof(struct energy_task), GFP_ATOMIC);
	if (e_task == NULL) {
		return NULL;
	}

	init_energy_task(e_task);

	/* Remember the task struct for the actual task. */
	e_task->task = task;

	/* Enqueue the created task in the global runqueue. */
	enqueue_energy_task(e_task);

	return e_task;
}

/* Free an energy task again.
 *
 * Requires that the lock of the global rq is taken.
 *
 * @e_task:	the energy task which should be freed again.
 */
static void free_energy_task(struct energy_task* e_task) {
	dequeue_energy_task(e_task);
	kfree(e_task);
}

/* Check if a thread is queued in the scheduling class runqueue.
 *
 * @t:		the task struct of the thread which should be tested.
 *
 * @returns:	whether or not the thread is queued in the runqueue or not.
 */
static inline bool thread_on_rq_queued(struct task_struct* t) {
	return t->ee.state & THREAD_RQ_QUEUED;
}

/* Check if a thread is queued in the CPU runqueue.
 *
 * @t:		the task struct of the thread which should be tested.
 *
 * @returns:	whether or not the thread is queued in the CPU runqueue or not.
 */
static inline bool thread_on_cpu_rq_queued(struct task_struct* t) {
	return t->ee.state & THREAD_CPU_QUEUED;
}

/* Check if a thread is currently running on a CPU.
 *
 * @t:		the task struc of the thread which should be tested.
 *
 * @returns:	whether or not the thread is currently running on a CPU.
 */
static inline bool thread_cpu_running(struct task_struct* t) {
	return t->ee.state & THREAD_RUNNING;
}

/* Enqueue a thread into the list of running threads of the energy task.
 *
 * @rq:		the runqueue of the current CPU.
 * @e_task:	the energy task struct of the energy task to which the thread
 *		belongs.
 * @t:		the task struct of the thread which should be enqueued.
 */
static void enqueue_runnable(struct rq* rq, struct energy_task* e_task, struct task_struct* t) {
	if (thread_on_rq_queued(t)) {
		/* This thread is already enqueued in the runqueue. */
		BUG();
	}

	list_add(&(t->ee.rq), &(e_task->runnable));
	e_task->nr_runnable++;

	t->ee.state |= THREAD_RQ_RUNNABLE;

	/* Remember in the global runqueue that we have a runnable thread. */
	grq.nr_threads++;

	/* Remember in the runqueue that there is now a new runnable linux task. */
	lock_local_rq(rq);
	__inc_nr_running(rq);
	unlock_local_rq(rq);
}

/* Enqueue a thread into the list of running threads of a CPU.
 *
 * Requires that the lock of the local runqueue is taken.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct fo the thread which should be enqueued.
 */
static void enqueue_running(struct rq* rq, struct task_struct* t) {
	if (thread_on_cpu_rq_queued(t)) {
		/* The thread is already enqueued in the runqueue. */
		BUG();
	}

	list_add(&(t->ee.cpu_rq), &(rq->en.runnable));
	rq->en.nr_runnable++;

	t->ee.state |= THREAD_CPU_RUNNABLE;
}

/* Dequeue a thread from the list of running threads of the energy task.
 *
 * @e_task:	the energy task struct of the energy task to which the thread
 *		belongs.
 * @t:		the task struct of the thread which should be dequeued.
 */
static void dequeue_runnable(struct energy_task* e_task, struct task_struct* t) {
	if (!thread_on_rq_queued(t)) {
		/* This thread is not enqueued in the runqueue. */
		BUG();
	}

	list_del(&(t->ee.rq));
	e_task->nr_runnable--;

	t->ee.state &= ~THREAD_RQ_RUNNABLE;

	/* Remember in the global runqueue that the thread is no longer runnable. */
	grq.nr_threads--;

	/* Remember in the runqueue that the thread is no longer runnable. */
	lock_local_rq(task_rq(t));
	__dec_nr_running(task_rq(t));
	unlock_local_rq(task_rq(t));
}

/* Dequeue a thread from the list of running threads on a CPU runqueue.
 *
 * Requires that the local runqueue lock is taken.
 *
 * @t:		the task struct of the thread which should be dequeued.
 */
static void dequeue_running(struct task_struct* t) {
	if (!thread_on_cpu_rq_queued(t)) {
		/* This thread is not enqueued in the runqueue. */
		BUG();
	}

	list_del(&(t->ee.cpu_rq));
	task_rq(t)->en.nr_runnable--;

	t->ee.state &= ~THREAD_CPU_QUEUED;
}

/* Calculate the time which the energy scheduling class should run.
 *
 * @returns:	the runtime for the energy scheduling class.
 */
static inline u64 sched_slice_class(void) {
	return grq.nr_threads * THREAD_SCHED_SLICE;
}

/* Calculate the time which the current energy task should run.
 *
 * @returns:	the runtime for the energy task.
 */
static inline u64 sched_slice_energy(struct energy_task* e_task) {
	/* The energy scheduling slice is simply the class scheduling slice
	 * distributed equally between the energy tasks. */
	return e_task == NULL ?
		0 :
		e_task->nr_runnable * THREAD_SCHED_SLICE;
}

/* Calculate the time which a thread assigned to CPU should run.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	the runtime for the current thread.
 */
static inline u64 sched_slice_local(struct rq* rq) {
	/* The local scheduling slice is simply the energy scheduling slice
	 * distributed equally between the threads assigned to one CPU. */
	return rq->en.nr_runnable == 0 ?
		sched_slice_energy(rq->en.curr_e_task) :
		sched_slice_energy(rq->en.curr_e_task) / rq->en.nr_runnable;
}

/* Calculate the time which other scheduling classes should run.
 *
 * @returns:	the runtime for other scheduling class.
 */
static inline u64 sched_slice_other(void) {
	return (nr_running() - grq.nr_threads) * THREAD_SCHED_SLICE;
}

/* Decide if we should switch to the energy sched class from another one.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	whether we should switch or not.
 */
static inline bool should_switch_to_energy(struct rq* rq) {
	if (grq.nr_threads == 0) {
		/* We have no threads to schedule currently. */
		return false;
	} else if (nr_running() == grq.nr_threads) {
		/* There are only threads of energy tasks in the system. */
		return true;
	} else if (nr_running() == rq->en.nr_assigned) {
		/* All other runqueues run the idle thread in this runqueue only has
		 * threads from energy tasks. */
		return true;
	} else if (nr_running() == 0) {
		/* Everyone runs the idle thread, but there are energy tasks available. */
		return true;
	} else {
		u64 now = rq_clock(rq);
		u64 not_running = now <= grq.stop_running ? 0 : now - grq.stop_running;

		return not_running > sched_slice_other();
	}
}

static inline bool should_check_cpus(void) {
	return grq.nr_tasks != 0;
}

/* Decide if we should switch away from the energy scheduling class to another
 * one.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	whether we should switch or not.
 */
static inline bool should_switch_from_energy(struct rq* rq) {
	if (grq.nr_threads == 0) {
		/* We have no threads to schedule currently. */
		return true;
	} else if (nr_running() == grq.nr_threads) {
		/* There are only threads of energy tasks in the system. */
		return false;
	} else {
		u64 now = rq_clock(rq);
		u64 running = now <= grq.start_running ? 0 : now - grq.start_running;

		return running > sched_slice_class();
	}
}

/* Decide if we should switch to another energy task.
 *
 * Requires that the global runqueue lock is taken.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	whether we should switch or not.
 */
static inline bool should_switch_in_energy(struct rq* rq) {
	if (grq.nr_tasks <= 1) {
		/* We can only switch between energy tasks if there are more than
		 * one energy task in the global runqueue.*/
		return false;
	} else if (rq->en.curr_e_task == NULL) {
		/* There is no current energy task any more, but we have other tasks
		 * available, so switch in any case. */
		return true;
	} else {
		/* Ok, we have more than one energy task. So decide based on the runtime
		 * of the energy task. */
		struct energy_task* e_task = rq->en.curr_e_task;

		u64 now = rq_clock(rq);
		u64 running = now <= e_task->start_running ? 0 : now - e_task->start_running;

		return running > sched_slice_energy(e_task);
	}
}

/* Decide if we should switch to another CPU local thread.
 *
 * Requires that the local runqueue lock is taken.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	whether we should switch or not.
 */
static inline bool should_switch_local(struct rq* rq) {
	if (rq->en.nr_runnable <= 1) {
		/* We can only switch locally if there are more than one thread assigned
		 * to this runqueue. */
		return false;
	} else if (rq->en.curr == NULL) {
		/* There is no current task any more, but we have other tasks available
		 * on the CPU, so switch in any case. */
		return true;
	} else {
		/* Ok, we have more than one thread assigned to this runqueue. So decide
		 * based on how long the thread was running if we should switch to another
		 * one or not. */
		struct task_struct* curr = rq->en.curr;
		u64 exec_time;

		exec_time = curr->se.sum_exec_runtime - curr->se.prev_sum_exec_runtime;

		return exec_time > sched_slice_local(rq);
	}
}

/* Decide whether we need to reassign the threads of an energy task to the available
 * CPU's.
 *
 * @e_task:	the energy task struct of the energy task which may needs to be
 *		redistributed.
 * @t:		the task struct of the thread of the energy task which changed its
 *		state.
 *
 * @retruns:	whether or not it is necessary to redistribute the enregy task.
 */
static inline bool should_redistribute_energy(struct energy_task* e_task,
		struct task_struct* t) {
	return e_task->state == ETASK_RUNNING || thread_cpu_running(t);
}

/* Tell the given runqueue to perform a local rescheduling.
 *
 * @rq:		the runqueue which should perform a local rescheduling.
 */
static inline void resched_curr_local(struct rq* rq) {
	rq->en.resched_flags |= LOCAL_RESCHED;

	set_tsk_need_resched(rq->curr);
}

/* Check if we must perform a local rescheduling.
 *
 * @rq:		the runqueue of the current CPU.
 *
 * @returns:	whether or not we must reschedule locally.
 */
static inline bool need_resched_curr_local(struct rq* rq) {
	return rq->en.resched_flags & LOCAL_RESCHED;
}

/* Clear the local reschedule flag again on the given runqueue.
 *
 * @rq:		the runqueue at which the flag should be cleared.
 */
static inline void clear_resched_curr_local(struct rq* rq) {
	rq->en.resched_flags &= ~LOCAL_RESCHED;
}

/* Update the energy statistics of an energy task.
 *
 * @e_task:	the energy task struct of the energy task.
 */
static void update_energy_statistics(struct energy_task* e_task) {
	struct task_struct* task = e_task->task;
	struct energy_statistics* stats = &(task->e_statistics);
	struct rapl_counters last_grc;
	u64 duration;

	copy_rapl_counters(&grc, &last_grc);
	duration = read_rapl_counters(&grc, true);

	stats->nr_updates++;
	stats->nr_defers++;
	stats->us_defered += duration;

	__update_rapl_counter(&(stats->uj_package), __diff_wa(grc.package, last_grc.package),
			duration, gri.loop_package);
	__update_rapl_counter(&(stats->uj_dram), __diff_wa(grc.dram, last_grc.dram),
			duration, gri.loop_dram);
	__update_rapl_counter(&(stats->uj_core), __diff_wa(grc.core, last_grc.core),
			duration, gri.loop_core);
	__update_rapl_counter(&(stats->uj_gpu), __diff_wa(grc.gpu, last_grc.gpu),
			duration, gri.loop_gpu);
}

/* Update the runtime statistics of a thread of an energy task.
 *
 * @rq:		the runqueue at which the thread did run.
 * @t:		the task struct of the thread.
 */
static void update_local_statistics(struct rq* rq, struct task_struct* t) {
	u64 now = rq_clock_task(rq);
	u64 delta_exec;

	if (unlikely(!t))
		return;

	/* Calculate how long the linux task has run. */
	delta_exec = now - t->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	t->se.exec_start = now;

	/* Update the maximum runtime. */
	schedstat_set(t->se.statistics.exec_max, max(delta_exec, t->se.statistics.exec_max));

	/* Increase the total runtime of the linux task. */
	t->se.sum_exec_runtime += delta_exec;

	/* Update the CPU accounting. */
	cpuacct_charge(t, delta_exec);

	/* Update the runtime average of the scheduler. */
	sched_rt_avg_update(rq, delta_exec);
}

/* Update the CPU assigned to the given linux task t.
 *
 * This method also properly handles the number of runnable linux tasks on
 * each of the CPU's runqueues.
 *
 * @t:		the task struct of the linux task for which the assigned CPU
 *		should be changed.
 * @cpu:	the new CPU number.
 */
static inline void move_local_task(struct task_struct* t, unsigned int cpu) {
	if (task_cpu(t) == cpu) {
		/* Do not move the task if it is already at the correct CPU. */
		return;
	}

	lock_local_rq(task_rq(t));
	__dec_nr_running(task_rq(t));
	unlock_local_rq(task_rq(t));

	__set_task_cpu(t, cpu);

	lock_local_rq(task_rq(t));
	__inc_nr_running(task_rq(t));
	unlock_local_rq(task_rq(t));
}

/* Set on the given runqueue that the given energy task is now running there.
 *
 * @rq:		the runqueue where the idle task should run.
 * @e_task:	the energy task which is currently running.
 */
static void set_energy_task(struct rq* rq, struct energy_task* e_task) {
	lock_local_rq(rq);

	rq->en.curr_e_task = e_task;

	unlock_local_rq(rq);
}

/* Set the thread to run on the current runqeueue.
 *
 * Requires that the lock of the local runqueue is taken.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the thread which should run next.
 */
static inline void set_local_task(struct rq* rq, struct task_struct* t) {
	t->ee.state |= THREAD_RUNNING;
	rq->en.curr = t;

	t->se.exec_start = rq_clock_task(rq);
	t->se.prev_sum_exec_runtime = t->se.sum_exec_runtime;
}

/* Tell all CPUs belonging to the current energy domain, that a new energy
 * task is going to run and hence which threads are assigned to them.
 *
 * Requires that the global runqueue lock is taken.
 *
 * @rq:		the runqueue of the current CPU.
 * @e_task:	the energy task which is going to be distributed.
 */
static void distribute_energy_task(struct rq* rq, struct energy_task* e_task) {
	/* Mark the energy task running. */
	e_task->state = ETASK_RUNNING;
	e_task->start_running = rq_clock(rq);

	/* Copy the current energy domain. */
	cpumask_copy(&(e_task->domain), &(rq->en.domain));

	__distribute_energy_task(e_task);
}

/* Assign a linux task t belonging to the energy task e_task to a special
 * runqueue.
 *
 * @rq:		the runqueue to which the linux task should be assigned.
 * @t:		the linux task which should be assigned.
 */
static void distribute_local_task(struct rq* rq, struct task_struct* t) {
	clear_resched_curr_local(rq);

	/* Update the CPU assigned to the local task. */
	move_local_task(t, cpu_of(rq));

	lock_local_rq(rq);

	/* Enqueue in the local runqueue. */
	enqueue_running(rq, t);

	unlock_local_rq(rq);
}

/* Reevaluate the task assignment after a new thread for an energy task arrived.
 *
 * Requires that the global runqueue lock is taken.
 *
 * @rq:		the runqueue of the current CPU.
 * @e_task:	the energy task struct of the energy task which should be redistributed.
 * @arrived:	whether the energy task has new threads or some if its threads vanished.
 */
static void redistribute_energy_task(struct rq* rq, struct energy_task* e_task, bool arrived) {
	if (arrived) {
		if (!grq.running) {
			__switch_to_energy(rq);
		}

		if (e_task->state != ETASK_RUNNING) {
			/* Make an internal switch. */
			switch_in_energy(rq, rq->en.curr_e_task, e_task);
		} else {
			/* The energy task is already running, so just redistribute it. */
			__distribute_energy_task(e_task);
		}
	} else {
		if (e_task->nr_runnable != 0) {
			/* The energy task still has threads to run, so just redistribute them. */
			__distribute_energy_task(e_task);
		} else {
			/* The energy task has no threads to run anymore. Remove the task. */
			put_energy_task(rq, e_task);

			if (grq.nr_tasks == 0 ) {
				__switch_from_energy(rq);
			}
		}
	}
}

/* Tell all CPUs belonging to the current energy domain, that the energy
 * task is not going to run any more.
 *
 * @e_task:	the energy task which is going to stop.
 */
static void clear_energy_task(struct energy_task* e_task) {
	int cpu;

	for_each_cpu(cpu, &(e_task->domain)) {
		clear_local_tasks(cpu_rq(cpu));
	}
}

/* Clear the locally assigned linux tasks at the given runqueue rq.
 *
 * Requires that the runqueue lock is taken.
 *
 * @rq:		the runqueue at which all local linux tasks should be
 *		removed.
 */
static void clear_local_tasks(struct rq* rq) {
	lock_local_rq(rq);

	/* Clear the lits of threads assigned to this CPU. */
	while (!list_empty(&(rq->en.runnable))) {
		struct task_struct* thread = list_first_entry(&(rq->en.runnable), struct task_struct, ee.cpu_rq);
		dequeue_running(thread);
	}
	rq->en.nr_runnable = 0;

	/* Reset the pointers to the currently running task and energy task. */
	rq->en.curr = NULL;
	rq->en.curr_e_task = NULL;

	clear_resched_curr_local(rq);

	unlock_local_rq(rq);

	/* Force rescheduling on the runqueue. */
	set_tsk_need_resched(rq->curr);
}

/* Remove the energy task e_task as currently running one.
 *
 * @rq:		the runqueue of the current CPU.
 * @e_task:	the energy task which should not run any more.
 */
static void put_energy_task(struct rq* rq, struct energy_task* e_task) {
	/* Update the energy task's statistics. */
	update_energy_statistics(e_task);

	/* Tell all CPUs to stop executing the threads of the current
	 * energy task. */
	clear_energy_task(e_task);

	e_task->state = 0;

	cpumask_clear(&(e_task->domain));

	/* Check if we can remove the energy task again. */
	if (e_task->nr_runnable == 0) {
		free_energy_task(e_task);
	}
}

/* Remove the linux task t as currently running one.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should not run
 *		any more.
 */
static void put_local_task(struct rq* rq, struct task_struct* t) {
	lock_local_rq(rq);

	update_local_statistics(rq, t);

	t->ee.state &= ~THREAD_RUNNING;
	rq->en.curr = NULL;

	unlock_local_rq(rq);
}

/* Pick a new energy task which should run next from the global runqueue.
 *
 * Requires that the global runqueue lock is taken.
 *
 * @returns:	the energy task which should run next.
 */
static struct energy_task* pick_next_energy_task(void) {
	struct energy_task* head = list_first_entry(&(grq.tasks), struct energy_task, rq);

	/* Go through the whole list by rotating it and try to find an energy task which is
	 * not running already but has runnable threads. */
	do {
		struct energy_task* next_e_task = list_first_entry(&(grq.tasks),
				struct energy_task, rq);
		list_rotate_left(&(grq.tasks));

		if ((next_e_task->state == 0) && (next_e_task->nr_runnable != 0)) {
			/* We have found our next energy task. */
			return next_e_task;
		}
	} while (head != list_first_entry(&(grq.tasks), struct energy_task, rq));

	/* We could not find any task. */
	return NULL;
}

/* Pick a new linux task which should run from the list of runnable task of the
 * given runqueue.
 *
 * @rq:		the runqueue from which a new task should be picked.
 */
static struct task_struct* pick_next_local_task(struct rq* rq) {
	struct task_struct* next;

	clear_resched_curr_local(rq);

	lock_local_rq(rq);

	if (rq->en.nr_runnable != 0) {
		/* We have threads available on the runqueue, so pick on of them. */
		next = list_first_entry(&(rq->en.runnable), struct task_struct, ee.cpu_rq);
		list_rotate_left(&(rq->en.runnable));
	} else {
		/* We have no threads to run, so run the idle task. */
		next = rq->en.idle;
	}

	/* Set that we are now executing the selected thread. */
	set_local_task(rq, next);

	unlock_local_rq(rq);

	return next;
}

static void acquire_cpus(struct cpumask* domain) {
	unsigned int cpu;

	for_each_cpu(cpu, domain) {
		struct rq* c_rq = cpu_rq(cpu);

		if (c_rq->en.blocked) {
			__acquire_cpu(c_rq);
		}
	}
 }

static void release_cpus(struct cpumask* domain) {
	unsigned int cpu;

	for_each_cpu(cpu, domain) {
		struct rq* c_rq = cpu_rq(cpu);

		if (!c_rq->en.blocked && c_rq->nr_running == c_rq->en.nr_assigned) {
			__release_cpu(c_rq);
		}
	}
}

static void check_cpus(struct cpumask* domain) {
	unsigned int cpu;

	for_each_cpu(cpu, domain) {
		struct rq* c_rq = cpu_rq(cpu);

		if (c_rq->en.blocked && c_rq->nr_running > 0) {
			__acquire_cpu(c_rq);
		} else if (!c_rq->en.blocked && c_rq->nr_running == c_rq->en.nr_assigned) {
			__release_cpu(c_rq);
		}
	}
}

/* Switch to the energy scheduling class from another scheduling class.
 *
 * @rq:		the runqueue of the current CPU.
 * @to:		the energy task which we switch to.
 */
static void switch_to_energy(struct rq* rq, struct energy_task* to) {
	if (to) {
		__switch_to_energy(rq);

		distribute_energy_task(rq, to);
	}
}

/* Switch from the energy scheduling class to another scheduling class.
 *
 * @rq:		the runqueue of the current CPU.
 * @from:	the energy task which we switch away from.
 */
static void switch_from_energy(struct rq* rq, struct energy_task* from) {
	if (from) {
		put_energy_task(rq, from);
	}

	__switch_from_energy(rq);
}

/* Switch from one energy task to another one within the energy scheduling
 * class.
 *
 * @rq:		the runqueue of the current CPU.
 * @from:	the energy task which we switch away from.
 * @to:		the energy task which we switch to.
 */
static void switch_in_energy(struct rq* rq, struct energy_task* from,
		struct energy_task* to) {
	if (from)
		put_energy_task(rq, from);

	if (to)
		distribute_energy_task(rq, to);
}

/* Set and initialize the energy domain of a given CPU.
 *
 * @domain:	the pointer to the cpumask where the energy domain should be
 *		set.
 * @cpu:	the CPU for which we need to initialize the energy domain.
 */
static void __init init_energy_domain(struct cpumask* domain, unsigned int cpu) {
	/* TODO: Really find the energy domain of the current CPU. */
	cpumask_setall(domain);
}

/* The idle thread function. */
static int idle_thread_fn(void* unused) {
	while (!kthread_should_stop()) {
		while (!tif_need_resched())
			cpu_relax();

		schedule();
	}

	return 0;
}

/* Start managing a linux task in our scheduling class.
 *
 * @pid:	the pid of the linux task which should be managed now.
 *
 * @returns:	0 on success or -ERROR on an error.
 */
static int do_start_energy(pid_t pid) {
	struct task_struct* p;
	int ret;

	if (pid < 0) {
		return -EINVAL;
	}

	ret = -ESRCH;
	p = pid == 0 ? current : find_task_by_vpid(pid);

	if (p) {
		/* Determine the actual task to the linux task and insert all its threads
		 * into the energy scheduling class. */
		struct task_struct* task = find_task(p);
		struct task_struct* thread;

		for_each_thread(task, thread) {
			struct sched_param param = { .sched_priority = 0 };

			ret = sched_setscheduler_nocheck(thread, SCHED_ENERGY, &param);
		}
	}

	return ret;
}

/* Stop managing a linux task in our scheduling class.
 *
 * @pid:	the pid of the linux task which should no longer be managed.
 *
 * @returns:	0 on success or -ERROR on an error.
 */
static int do_stop_energy(pid_t pid) {
	struct task_struct* p;
	int ret;

	if (pid < 0) {
		return -EINVAL;
	}

	ret = -ESRCH;
	p = pid == 0 ? current : find_task_by_vpid(pid);

	if (p) {
		/* Determine the actual task to the linux task and remove all its threads
		 * from the energy scheduling class. */
		struct task_struct* task = find_task(p);
		struct task_struct* thread;

		for_each_thread(task, thread) {
			struct sched_param param = { .sched_priority = 0 };

			ret = sched_setscheduler_nocheck(thread, SCHED_NORMAL, &param);
		}
	}

	return ret;
}


/***
 * External function definitions.
 ***/

/* Add a linux task t to the runqueue.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should be added.
 * @flags:
 */
void enqueue_task_energy(struct rq* rq, struct task_struct* t, int flags) {
	struct energy_task* e_task;

	lock_grq();

	e_task = find_energy_task(t);

	if (e_task == NULL) {
		/* Ok, the energy task did not exist yet, so we need to create it first,
		 * before we can continue. */
		if (!(e_task = create_energy_task(t))) {
			BUG();
		}
	}

	/* Add the thread to the list of runnable threads. */
	enqueue_runnable(rq, e_task, t);

	if (should_redistribute_energy(e_task, t)) {
		redistribute_energy_task(rq, e_task, true);
	}

	unlock_grq();
}

/* Remove a linux task t from the runqueue.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should be removed.
 * @flags:
 */
void dequeue_task_energy(struct rq* rq, struct task_struct* t, int flags) {
	struct energy_task* e_task;

	lock_grq();

	e_task = find_energy_task(t);

	if (e_task == NULL) {
		/* This should not happen. */
		BUG();
	}

	/* Remove the thread from the CPU runqueue. */
	if (thread_on_cpu_rq_queued(t)) {
		lock_local_rq(task_rq(t));

		dequeue_running(t);

		unlock_local_rq(task_rq(t));
	}

	/* Remove the thread from the list of runnable threads. */
	dequeue_runnable(e_task, t);

	if (should_redistribute_energy(e_task, t)) {
		redistribute_energy_task(rq, e_task, false);
	}

	unlock_grq();
}

/* The currently running linux task wants to give up the CPU.
 *
 * @rq:		the runqueue of the current CPU.
 */
void yield_task_energy(struct rq* rq) {
	if (rq->en.nr_runnable > 2) {
		/* Yield in this scheduling class will only work if multiple
		 * threads of the same task are assigned to the same CPU. If
		 * this is the case, a local rescheduling is performed. */
		resched_curr_local(rq);
	}
}

/* The current running linux task wants to give up the CPU to another linux task t.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should run next.
 * @preemt:	whether or not preempting of another linux task is allowed.
 *
 * @returns:	whether we yielded to the other task or not.
 */
bool yield_to_task_energy(struct rq* rq, struct task_struct* t, bool preemt) {
	/* TODO: Implement yield to. */
	return false;
}

/* Preempt the current linux task in favor of the linux task t.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should be run in favor
 *		of the current one.
 * @flags:
 */
void check_preempt_curr_energy(struct rq* rq, struct task_struct* t, int flags) {
	/* We are doing nothing here. The currently running task is never preempted
	 * in favor of another one. */
	return;
}

/* Select a new linux task which should run instead of prev.
 *
 * @rq:		the runqueue of the current CPU.
 * @prev:	the task struct of the liunx task which should be replaced.
 *
 * @returns:	the task struct of the linux task which should run next.
 */
struct task_struct* pick_next_task_energy(struct rq* rq, struct task_struct* prev) {
	lock_grq();

	if (!grq.running) {
		if (should_switch_to_energy(rq))
			switch_to_energy(rq, pick_next_energy_task());
		else if (should_check_cpus())
			check_cpus(&(rq->en.domain));
	} else {
		struct energy_task* curr_e_task = rq->en.curr_e_task;

		if (should_switch_from_energy(rq))
			switch_from_energy(rq, curr_e_task);
		else if (should_switch_in_energy(rq))
			switch_in_energy(rq, curr_e_task, pick_next_energy_task());
	}

	unlock_grq();

	if (need_resched_curr_local(rq)) {
		/* Tell the scheduling class of prev that it is going to be removed. */
		put_prev_task(rq, prev);

		/* Select a new thread which should run on this CPU. */
		pick_next_local_task(rq);
	}

	return rq->en.curr;
}

/* Tell the scheduling class that the linux task t is going to lose its CPU share.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task which should give up the CPU.
 */
void put_prev_task_energy(struct rq* rq, struct task_struct* t) {
	put_local_task(rq, t);
}

/* Tell the scheduling class, that the currently running task will continue running.
 *
 * @rq:		the runqueue of the current CPU.
 */
void set_curr_task_energy(struct rq* rq) {
	struct task_struct* curr = rq->curr;

	lock_local_rq(rq);

	if (!thread_on_rq_queued(curr)) {
		enqueue_running(rq, curr);
	}
	set_local_task(rq, curr);

	unlock_local_rq(rq);
}

/* A scheduling tick happened with the linux task t running.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task for which the scheduling tick
 *		happened.
 * @queued:	is the task still in a runqueue.
 */
void task_tick_energy(struct rq* rq, struct task_struct* t, int queued) {
	update_local_statistics(rq, t);

	lock_grq();

	if (should_switch_in_energy(rq) || should_switch_from_energy(rq)) {
		resched_curr(rq);
	}

	unlock_grq();

	lock_local_rq(rq);

	if (should_switch_local(rq)) {
		resched_curr_local(rq);
	}

	unlock_local_rq(rq);
}

/* The linux task t was just created by a fork.
 *
 * @t:		the task struct of the linux task which just was created.
 */
void task_fork_energy(struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The linux task t died.
 *
 * @t:		the task struct of the linux task which just died.
 */
void task_dead_energy(struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The scheduling class of the linux task t changed to another one.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the linux task which was removed from our scheduling class.
 */
void switched_from_energy(struct rq* rq, struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The scheduling class of the linux task t changed to this one.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the linux task which was added to our scheduling class.
 */
void switched_to_energy(struct rq* rq, struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The priority of the linux task t changed.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task for which the priority was changed.
 * @old_prio:	the previous priority of the task.
 */
void prio_changed_energy(struct rq* rq, struct task_struct* t, int old_prio) {
	/* We have nothing to do here currently. */
	return;
}

/* Get the round robin interval for the linux task t.
 *
 * This information is important for the POSIX RR-Scheduler.
 *
 * @rq:		the runqueue of the current CPU.
 * @t:		the task struct of the linux task for which the RR interval should
 *		be returned.
 *
 * @returns:	the RR interval for the given linux task.
 */
unsigned int get_rr_interval_energy(struct rq* rq, struct task_struct* t) {
	return sched_slice_local(rq);
}

/* Update the runtime statistics of the currently running linux task outside
 * of a schedule tick.
 *
 * @rq:		the runqueue of the current CPU.
 */
void update_curr_energy(struct rq* rq) {
	update_local_statistics(rq, rq->curr);
}

/* Select the CPU where the linux task t should run.
 *
 * @t:		the task struct of the linux task for which a new runqueue should be
 *		selected.
 * @cpu:	the CPU where it is currently running.
 * @sd_flags:	scheduler flags such as SD_BALANCE_FORK.
 * @flags:	other flags like wake_up_flags.
 *
 * @returns:	the CPU number where the linux task t should run.
 */
int select_task_rq_energy(struct task_struct* t, int cpu, int sd_flags, int flags) {
	return cpu;
}

/* The linux task t is going to be migrated to a new CPU.
 *
 * @t:		the task struct of the linux task which is going to be migrated.
 * @new_cpu:	the new CPU where the linux task will be migrated to.
 */
void migrate_task_rq_energy(struct task_struct* t, int new_cpu) {
	/* We have nothing to do here currently. */
	return;
}

/* The linux task t is going to be woken up.
 *
 * @t:		the task struct of the linux task which is going to be woken up.
 */
void task_waking_energy(struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The linux task t was woken up.
 *
 * @rq:		the runqueue where the linux task was woken up.
 * @t:		the task struct of the linux task which was woken up.
 */
void task_woken_energy(struct rq* rq, struct task_struct* t) {
	/* We have nothing to do here currently. */
	return;
}

/* The CPUs where the linux task t is allowed to run changed.
 *
 * @t:		the task struct of the linux task which changed its CPUs.
 * @newmask:	the new CPUs where the linux task is allowed to run.
 */
void set_cpus_allowed_energy(struct task_struct* t, const struct cpumask* newmask) {
	/* TODO: Check if we need to reschedule the corresponding energy task. */
	return;
}

/* A CPU was plugged in and became online.
 *
 * @rq:		the runqueue of the CPU which just came online.
 */
void rq_online_energy(struct rq* rq) {
	/* TODO: Check if we can reschedule the current energy task. */
	return;
}

/* A CPU was plugged out and became offline.
 *
 * @rq:		the runqueue of the CPU which just came offline.
 */
void rq_offline_energy(struct rq* rq) {
	/* TODO: Check if we need to reschedule the current energy task. */
	return;
}


/***
 * The Energy Scheduling Class.
 ***/

const struct sched_class energy_sched_class = {
	.next = &fair_sched_class,

	.enqueue_task = enqueue_task_energy,
	.dequeue_task = dequeue_task_energy,

	.yield_task = yield_task_energy,
	.yield_to_task = yield_to_task_energy,

	.check_preempt_curr = check_preempt_curr_energy,

	.pick_next_task = pick_next_task_energy,

	.put_prev_task = put_prev_task_energy,

	.set_curr_task = set_curr_task_energy,

	.task_tick = task_tick_energy,
	.task_fork = task_fork_energy,
	.task_dead = task_dead_energy,

	.switched_from = switched_from_energy,
	.switched_to = switched_to_energy,
	.prio_changed = prio_changed_energy,

	.get_rr_interval = get_rr_interval_energy,

	.update_curr = update_curr_energy,

#ifdef CONFIG_SMP
	.select_task_rq = select_task_rq_energy,
	.migrate_task_rq = migrate_task_rq_energy,

	.task_waking = task_waking_energy,
	.task_woken = task_woken_energy,

	.set_cpus_allowed = set_cpus_allowed_energy,

	.rq_online = rq_online_energy,
	.rq_offline = rq_offline_energy,
#endif
};

/***
 * Other external functions.
 ***/

/* Initialize the per core runqueues.
 *
 * @e_rq:	the energy runqueue which must be initialized.
 * @cpu:	the CPU it which this runqueue is established.
 */
void __init init_e_rq(struct e_rq* e_rq, unsigned int cpu) {
	raw_spin_lock_init(&(e_rq->lock));

	e_rq->resched_flags = 0;

	init_energy_domain(&(e_rq->domain), cpu);

	INIT_LIST_HEAD(&(e_rq->runnable));
	e_rq->nr_runnable = 0;

	e_rq->nr_assigned = 0;

	e_rq->blocked = false;

	e_rq->curr = NULL;
	e_rq->curr_e_task = NULL;

	e_rq->idle = NULL;
}

/* Initialize the idle threads for each available runqueue. */
int __init init_e_idle_threads(void) {
	int cpu;

	for_each_possible_cpu(cpu) {
		struct task_struct* idle_thread;
		struct rq* c_rq = cpu_rq(cpu);

		/* Create the kernel thread and move it into our scheduling
		 * class. */
		idle_thread = kthread_create(idle_thread_fn, NULL, "e_idle/%u", cpu);
		kthread_bind(idle_thread, cpu);

		idle_thread->sched_class = &energy_sched_class;
		idle_thread->policy = SCHED_ENERGY;
		idle_thread->state = TASK_RUNNING;

		c_rq->en.idle = idle_thread;
	}

	return 0;
}

late_initcall(init_e_idle_threads);

/* Initialize the RAPL subsystem. */
int __init init_rapl_subsystem(void) {
	init_gri();
	init_grc();

	printk(KERN_INFO "RAPL-subsystem initialized: %u %u %u %u %u\n",
			gri.update_interval, gri.loop_package, gri.loop_dram,
			gri.loop_core, gri.loop_gpu);

	return 0;
}

late_initcall(init_rapl_subsystem);

/* Initialize the energy scheduling class. */
void __init init_sched_energy_class(void) {
	init_grq();
}

/* The system call to start energy measurements.
 *
 * @pid:	the pid of the linux task which should be measured.
 *
 * @returns:	0 on success or -ERROR on an error.
 */
SYSCALL_DEFINE1(start_energy, pid_t, pid) {
	return do_start_energy(pid);
}


/* The system call to stop energy measurements.
 *
 * @pid:	the pid of the linux task which should not be measured
 *		any more.
 *
 * @returns:	0 on success or -ERROR on an error.
 */
SYSCALL_DEFINE1(stop_energy, pid_t, pid) {
	return do_stop_energy(pid);
}
