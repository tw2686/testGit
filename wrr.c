#include "sched.h"
#include <linux/sched/wrr.h>
#include <linux/syscalls.h>
const struct sched_class wrrSchedClass;

void initWrrRq(struct wrrRq *wrrRq)
{
	wrrRq->nrRunning = 0;
	wrrRq->totalWeight = 0;
	INIT_LIST_HEAD(&wrrRq->rq);
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void updateCurrWrr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 deltaExec;

	if (curr->sched_class != &wrrSchedClass)
		return;

	/* get the runtime since start */
	deltaExec = rq_clock_task(rq) - curr->se.exec_start;

	if (unlikely((s64)deltaExec <= 0))
		return;

	/* 
 	 * if delteExec is greater, 
	 * update exec_max to deltaExec 
	 */
	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max,deltaExec));

	/* update the total time spent on the cpu in nanoseconds */
	curr->se.sum_exec_runtime += deltaExec;

	/*
	 * Maintain exec runtime for a thread group 
	 * by incrementing the sum_exec_runtime field
	 * by deltaExec. If not being maintained, get
	 * the struct for the running cpu and update the 
	 * field there
  	 */
	account_group_exec_runtime(curr,deltaExec);

	/* reset the start time */
	curr->se.exec_start = rq_clock_task(rq);

	/* 
	 * charge this task's execution time to
	 * its accounting group, 
	 * called with rq->lock held
	 */
	cpuacct_charge(curr,deltaExec);
}

/* 
 * Preempt the current task with a newly woken task if needed 
 * wrr does not enforce priority, hence empty 
 */
static void checkPreemtCurrWrr(struct rq *rq, struct task_struct *p, int flags)
{
	/* >_< */
}

/* Adding/removing a task to/from a run queue */
static void
enqueueTaskWrr(struct rq *rq, struct task_struct *p, int flags)
{
	/*
	 * flags - a bit vector of flags that describe 'why'
	 * 	   enqueue is being called
	 *	   eg) when a child process is first forked
	 *	       'enqueue_task' is called to put in on
  	 *	       a run-queue  
	 * ENQUEUE_WAKEUP - task just became runnable
	 * ENQUEUE_HEAD - place at front of runqueue(tail if not specified)
	 */

	struct schedWrrEntity *wrrSe = &p->wrr;

	/* flags tested using bitwise & */
	if (flags & ENQUEUE_WAKEUP)
		wrrSe->timeout = 0;	
	if (flags & ENQUEUE_HEAD)
		list_add(&wrrSe->runList,&rq->wrr.rq);
	else
		list_add_tail(&wrrSe->runList,&rq->wrr.rq);
	++rq->wrr.nrRunning;
	rq->wrr.totalWeight += wrrSe->weight;	
	add_nr_running(rq,1);
}

static void dequeueTaskWrr(struct rq *rq, struct task_struct *p, int flags) 
{
	/*
	 * flags - a bit vector of flags that describe 'why'
	 * 	   dequeue is being called
	 *	   eg) when a process exists, 'dequeue_task' is being
	 *	   called to take it off the run-queue
 	 */
	/* Update the current task's runtime statistics before dequeue */ 
	if(rq->wrr.nrRunning == 0)
		return;
	updateCurrWrr(rq);
	list_del_init(&p->wrr.runList);
	rq->wrr.totalWeight -= p->wrr.weight;
	--rq->wrr.nrRunning;
	sub_nr_running(rq,1);
}

/* Called right before p is taken off the CPU */
static void putPrevTaskWrr(struct rq *rq, struct task_struct *p)
{
	/* 
	 * class-specific implementation 
	 * rt uses this function as an opportunity to perform simple
	 * bookeeping while cfs keeps the currently running task out
	 * of its RB tree. In WRR, it simply updates the currently
	 * running task's runtime statistics
	 */
	updateCurrWrr(rq);
}

/* 
 * Called by scheduler to determine which of 
 * rq's tasks should be running. 
 * The name is a bit misleading; it's not supposed to return the task
 * that should run after the currently running task; instead it is
 * supposed to return the 'task_struct' that should be running now
 * in this instant
 */
static struct task_struct *
pickNextTaskWrr(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct schedWrrEntity *wrrSe = NULL;
	struct task_struct *p = NULL;
	if (!rq->wrr.nrRunning)
		return NULL;
	/* 
	 * WRR does not enforce priority 
	 * so, the first entry in the queue will be picked 
	 * Usually though, the kernel will context-switch
	 * from the task specified by prev
	 */
	putPrevTaskWrr(rq,prev);
	wrrSe = list_first_entry(&rq->wrr.rq, 
				 struct schedWrrEntity,
				 runList);
	p = container_of(wrrSe,struct task_struct,wrr);
	if(p == NULL)
		return NULL;

	/* set the start time for stats */ 
	p->se.exec_start = rq_clock_task(rq);
	return p;
}


/* Called when a task changes its scheduling class or changes its task group */
static void setCurrTaskWrr(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	p->se.exec_start = rq_clock_task(rq);
}
#ifdef CONFIG_POSIX_TIMERS
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft,hard;
	soft = task_rlimit(p,RLIMIT_RTTIME);
	hard = task_rlimit(p,RLIMIT_RTTIME);
	if (soft != RLIM_INFINITY) {	
		unsigned long next;
		++p->wrr.timeout;
		next = DIV_ROUND_UP(min(soft,hard),USEC_PER_SEC/HZ);
		if(p->wrr.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}	
}
#else
static void watchdog(struct rq *rq, struct task_struct *p) {}
#endif

/* Put the task to the head or the tail of the queue */
static void requeueTaskWrr(struct rq *rq, struct task_struct *p, int head)
{
	struct schedWrrEntity *wrrSe = &p->wrr;
	if (head)
		list_move(&wrrSe->runList,&rq->wrr.rq);
	else
		list_move_tail(&wrrSe->runList,&rq->wrr.rq);			
}

static void yieldTaskWrr(struct rq *rq)
{
	requeueTaskWrr(rq,rq->curr,0);
}

/* 
 * This function gets called whenever a timer interrupt happens
 * and its job is to perform bookeeping
 */
static void taskTickWrr(struct rq *rq, struct task_struct *p, int queued)
{
	struct schedWrrEntity *wrrSe = &p->wrr;

	/* update the current task's runtime statistics */
	updateCurrWrr(rq);
	
	watchdog(rq,p); 

	/* 
	 * Unlike rt scheduler of which WRR is based off
	 * WRR has only one policy, so policy is always SCHED_WRR
	 */
	if (p->policy != SCHED_WRR)
		return;

	/* if there is still time left for the task, then return */
	if(--p->wrr.timeSlice)
		return;
	
	/* 
	 * time slice expired, assign new time and put it to
	 * the head or the end of the queue 
	 */ 
	p->wrr.timeSlice = p->wrr.weight * WRR_TIMESLICE; 

	if (wrrSe->runList.prev != wrrSe->runList.next) {
		/* requeue the task to the queue */
		requeueTaskWrr(rq,p,0); 
		/* mark rq's current task 'to be rescheduled now' */	
		resched_curr(rq);
		return;
	}
}
static unsigned int getRrIntervalWrr(struct rq *rq, struct task_struct *task)
{
	/* 
	 * Unlike rt scheduler of which WRR is based off
	 * WRR has only one policy, so it is always true
	 */
	if (task->policy == SCHED_WRR)
		return task->wrr.weight * WRR_TIMESLICE;
	else
		return 0;
}

/*WRR does not enforce priority, hence empty */
static void
prioChangedWrr(struct rq *rq, struct task_struct *p, int oldprio)
{
	/* ~.~ */
}

static void switchedToWrr(struct rq *rq, struct task_struct *p)
{
	if (task_on_rq_queued(p)) {
		/*
		 * We were most likely switched from sched_rt, so
		 * kick off the schedule if running, otherwise just see
		 * if we can still preempt the current task.
		 */
		if (rq->curr == p)
			resched_curr(rq);
		else
			check_preempt_curr(rq,p,0);
	}
}

#ifdef CONFIG_SMP
/* returns an integer corresponding to the CPU with the loweset weight */ 
static int findLowestWeightCpu(void)
{
	int i;
	int res = -1;
	unsigned int minWeight = -1;
	struct wrrRq *rq;

	for_each_possible_cpu(i) {
		/* on each iteration, rq points to the rq of the CPU */
		rq = &cpu_rq(i)->wrr;
		if (rq->totalWeight < minWeight) {
			minWeight = rq->totalWeight;
			res = i;
		}	
	}
	return res;
}

static int
selectTaskRqWrr(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	/*
	 * Used by the core scheduler for distributing 
	 * processes accross multiple CPUs
	 * CPU assignment occurs in instances such as
	 * 	When a process is first forked,
	 * 	When a task is woken up after having gone to sleep,
	 * 	In response to any of the syscalls in the execv family
	 *	And many other more places
	 */

	/* For anything but wakeups, just return the task cpu */
	if(sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;
	rcu_read_lock();

	/* critical section begins */
	cpu = findLowestWeightCpu();
	/* critical section ends */
	rcu_read_unlock();
out:
	return cpu;
}

#endif
const struct sched_class wrrSchedClass = {
	.next = &fair_sched_class,
	.enqueue_task = enqueueTaskWrr,
	.dequeue_task = dequeueTaskWrr,
	.check_preempt_curr = checkPreemtCurrWrr,
	.pick_next_task = pickNextTaskWrr,
	.put_prev_task = putPrevTaskWrr,
#ifdef CONFIG_SMP
	.select_task_rq = selectTaskRqWrr,
	.set_cpus_allowed = set_cpus_allowed_common,
#endif
	.set_curr_task = setCurrTaskWrr,
	.task_tick = taskTickWrr,
	.get_rr_interval = getRrIntervalWrr,
	.prio_changed = prioChangedWrr,
	.switched_to = switchedToWrr,
	.update_curr = updateCurrWrr,
	.yield_task = yieldTaskWrr,
};
SYSCALL_DEFINE1(get_wrr_info, struct wrr_info __user *, info)
{
	struct wrr_info buff;
	struct wrrRq *wrr;
	int i;
	buff.num_cpus = 0;
	
	if(info == NULL)
		return -EINVAL;
	/* prevent the online CPU map changing while iterating */
	get_online_cpus();
	/* critical section begins */
	rcu_read_lock();
	
	for_each_online_cpu(i) {
		wrr = &cpu_rq(i)->wrr;
		buff.nr_running[buff.num_cpus] = wrr->nrRunning;
		buff.total_weight[buff.num_cpus] = wrr->totalWeight;
		++buff.num_cpus;
	}
	/* critical section ends */
	rcu_read_unlock();
	put_online_cpus();
	
	if(copy_to_user(info,&buff,sizeof(buff)))
		return -EFAULT;
	
	return 0;
}
SYSCALL_DEFINE1(set_wrr_weight, int, weight)
{
	/* TODO - check for root */
	struct rq *rq;
	struct rq_flags rf;

	if (weight < 1)
		return -EINVAL;
	if (current->sched_class != &wrrSchedClass)
		return -EACCES;
	rq = task_rq_lock(current,&rf);
	update_rq_clock(rq);	
	rq->wrr.totalWeight -= current->wrr.weight;
	rq->wrr.totalWeight += weight;
	current->wrr.weight = weight;
 	task_rq_unlock(rq,current,&rf);	
	return 0;
}
