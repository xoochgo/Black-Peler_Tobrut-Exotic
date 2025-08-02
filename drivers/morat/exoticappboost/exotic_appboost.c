// SPDX-License-Identifier: GPL-2.0
/*
 * Morat Engine - ExoticAppBoost
 * Boost only newly started user-installed apps to big core
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm_types.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/ktime.h>

#define BOOST_DURATION_MS 1000
#define BOOST_INTERVAL_MS 300
#define UID_APP_START 10000
#define MAX_BOOSTED_TASKS 64

static struct task_struct *boost_thread;
static int big_core = -1;

struct boosted_task {
	pid_t pid;
	ktime_t last_boost;
};

static struct boosted_task boosted[MAX_BOOSTED_TASKS];
static int boost_count = 0;

static bool was_recently_boosted(pid_t pid)
{
	ktime_t now = ktime_get();
	for (int i = 0; i < boost_count; i++) {
		if (boosted[i].pid == pid) {
			if (ktime_ms_delta(now, boosted[i].last_boost) < BOOST_DURATION_MS)
				return true;

			// refresh timestamp
			boosted[i].last_boost = now;
			return false;
		}
	}
	if (boost_count < MAX_BOOSTED_TASKS) {
		boosted[boost_count].pid = pid;
		boosted[boost_count].last_boost = now;
		boost_count++;
	}
	return false;
}

static bool is_user_installed_app(struct task_struct *p)
{
	const struct cred *cred = p->cred;
	if (!p->mm || !cred)
		return false;

	uid_t uid = from_kuid(&init_user_ns, cred->uid);
	return uid >= UID_APP_START;
}

static void boost_task_to_big_core(struct task_struct *p)
{
	struct cpumask mask;

	if (!p || big_core < 0 || big_core >= nr_cpu_ids)
		return;

	cpumask_clear(&mask);
	cpumask_set_cpu(big_core, &mask);

	set_cpus_allowed_ptr(p, &mask);
	pr_info("ExoticAppBoost: Boosted %s (pid %d) to CPU%d\n", p->comm, p->pid, big_core);
}

static int boost_thread_fn(void *data)
{
	while (!kthread_should_stop()) {
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(p) {
			if (is_user_installed_app(p) && !was_recently_boosted(p->pid)) {
				boost_task_to_big_core(p);
			}
		}
		rcu_read_unlock();

		msleep(BOOST_INTERVAL_MS);
	}
	return 0;
}

static int __init exotic_appboost_init(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu_max_freq(cpu) > 2000000) {
			big_core = cpu;
			break;
		}
	}

	if (big_core == -1) {
		pr_info("ExoticAppBoost: No big core found\n");
		return -ENODEV;
	}

	boost_thread = kthread_run(boost_thread_fn, NULL, "exotic_appboost");
	if (IS_ERR(boost_thread)) {
		pr_info("ExoticAppBoost: Failed to start thread\n");
		return PTR_ERR(boost_thread);
	}

	pr_info("ExoticAppBoost: Initialized (boosting to CPU%d)\n", big_core);
	return 0;
}

static void __exit exotic_appboost_exit(void)
{
	if (boost_thread)
		kthread_stop(boost_thread);
	pr_info("ExoticAppBoost: Unloaded\n");
}

module_init(exotic_appboost_init);
module_exit(exotic_appboost_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mr. Morat");
MODULE_DESCRIPTION("Morat Engine - Boost installed apps at launch time");