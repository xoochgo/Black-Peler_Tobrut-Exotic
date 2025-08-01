// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define BALANCE_INTERVAL_MS 1000
#define MIN_DELTA_IRQS 64

extern unsigned int kstat_irqs_cpu(unsigned int irq, int cpu);

/* Morat Engine: ExoticBalance */
static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Enable or disable ExoticBalance");

static struct delayed_work balance_work;
static unsigned int *cpu_irq_count;
static unsigned int *cpu_irq_last;

static void exotic_balance_irq(struct work_struct *work)
{
	int irq, cpu;
	unsigned int max_irq = 0, min_irq = UINT_MAX;
	int max_cpu = -1, min_cpu = -1;

	if (!enabled) {
		schedule_delayed_work(&balance_work, msecs_to_jiffies(BALANCE_INTERVAL_MS));
		return;
	}

	memset(cpu_irq_count, 0, sizeof(unsigned int) * nr_cpu_ids);

	for (irq = 0; irq < nr_irqs; irq++) {
		for_each_online_cpu(cpu)
			cpu_irq_count[cpu] += kstat_irqs_cpu(irq, cpu);
	}

	for_each_online_cpu(cpu) {
		unsigned int delta = cpu_irq_count[cpu] - cpu_irq_last[cpu];

		if (delta > max_irq) {
			max_irq = delta;
			max_cpu = cpu;
		}
		if (delta < min_irq) {
			min_irq = delta;
			min_cpu = cpu;
		}

		cpu_irq_last[cpu] = cpu_irq_count[cpu];
	}

	pr_info("ExoticBalance: CPU%d busiest (%u irqs), CPU%d idlest (%u irqs)\n",
	        max_cpu, max_irq, min_cpu, min_irq);

	if (max_irq - min_irq >= MIN_DELTA_IRQS) {
		pr_info("ExoticBalance: balancing trigger (delta=%u)\n", max_irq - min_irq);
		// TODO: implement IRQ migration or affinity hint here
	}

	schedule_delayed_work(&balance_work, msecs_to_jiffies(BALANCE_INTERVAL_MS));
}

static int __init exoticbalance_init(void) __init;
static int __init exoticbalance_init(void)
{
	cpu_irq_count = kzalloc(sizeof(unsigned int) * nr_cpu_ids, GFP_KERNEL);
	cpu_irq_last = kzalloc(sizeof(unsigned int) * nr_cpu_ids, GFP_KERNEL);
	if (!cpu_irq_count || !cpu_irq_last)
		return -ENOMEM;

	INIT_DELAYED_WORK(&balance_work, exotic_balance_irq);
	schedule_delayed_work(&balance_work, msecs_to_jiffies(BALANCE_INTERVAL_MS));

	pr_info("ExoticBalance: initialized\n");
	return 0;
}

static void __exit exoticbalance_exit(void) __exit;
static void __exit exoticbalance_exit(void)
{
	cancel_delayed_work_sync(&balance_work);
	kfree(cpu_irq_count);
	kfree(cpu_irq_last);
	pr_info("ExoticBalance: unloaded\n");
}

module_init(exoticbalance_init);
module_exit(exoticbalance_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobrut Exotic");
MODULE_DESCRIPTION("ExoticBalance IRQ balancer with FKM toggle");