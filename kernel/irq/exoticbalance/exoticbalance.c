// SPDX-License-Identifier: GPL-2.0
// Property of Morat Engine

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/string.h>

#define BALANCE_INTERVAL_MS 8000
#define MIN_DELTA_IRQS_BASE 800
#define MAX_CPU_TEMP_THRESHOLD 70

extern unsigned int kstat_irqs_cpu(unsigned int irq, int cpu);

static bool exoticbalance_enabled = true;
module_param(exoticbalance_enabled, bool, 0644);
MODULE_PARM_DESC(exoticbalance_enabled, "Enable ExoticBalance");

static struct delayed_work balance_work;
static unsigned int *cpu_irq_count;
static unsigned int *cpu_irq_last;

/* IRQ blacklist berbasis nama (komprehensif dan aman) */
static const char *const irq_name_blacklist[] = {
	// Display / GPU / UI
	"mdss",         // Qualcomm display
	"sde",          // Snapdragon Display Engine
	"dsi",          // Display serial interface
	"mipi",         // MIPI interface
	"kgsl",         // GPU (Adreno)
	"adreno",       // GPU
	"msm_gpu",      // GPU varian lain

	// Input / Touchscreen
	"input",        // General input
	"touch",        // Generic touch
	"synaptics",    // Synaptics
	"fts",          // FocalTech
	"goodix",       // Goodix

	// Storage
	"ufs",          // UFS base
	"ufshcd",       // Host controller
	"qcom-ufshcd",  // Qualcomm variant
	"sdc",          // SD/MMC

	// Network / Internet
	"wlan",         // Wi-Fi
	"wifi",         // Alternate Wi-Fi
	"rmnet",        // Mobile data
	"ipa",          // Packet accelerator
	"qcom,sps",     // Internet DMA
	"bam",          // Bus Access Manager
	"modem",        // Modem IRQ
	"qrtr",         // Qualcomm RPC router

	// Charging / Power
	"pmic",         // Power management
	"smb",          // SMB135x/PMIC charger
	"bms",          // Battery monitor

	// Critical system
	"timer",        // System timer
	"hrtimer",      // High-resolution timer
	"watchdog",     // Watchdog
	"thermal",      // Thermal control
	"cpu",          // CPU related IRQ
	NULL
};

static bool is_irq_blacklisted(int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	const char *name;
	int i;

	if (!desc || !desc->action || !desc->action->name)
		return false;

	name = desc->action->name;
	for (i = 0; irq_name_blacklist[i] != NULL; i++) {
		if (strnstr(name, irq_name_blacklist[i], strlen(name)))
			return true;
	}

	return false;
}

static int get_max_cpu_temp(void)
{
#if IS_ENABLED(CONFIG_THERMAL)
	struct thermal_zone_device *tz;
	int temp = 0;

	tz = thermal_zone_get_zone_by_name("cpu-thermal");
	if (!IS_ERR(tz)) {
		tz->ops->get_temp(tz, &temp);
		temp /= 1000;
	}
	return temp;
#else
	return 0;
#endif
}

static unsigned int get_cpu_max_freq(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	unsigned int freq = 0;

	if (policy) {
		freq = policy->cpuinfo.max_freq;
		cpufreq_cpu_put(policy);
	}
	return freq;
}

static bool is_cpu_big(int cpu)
{
	return get_cpu_max_freq(cpu) >= 2000000;
}

static void migrate_irqs_simple(int from, int to)
{
	int irq;
	cpumask_t new_mask;

	for (irq = 0; irq < nr_irqs; irq++) {
		if (!irq_can_set_affinity(irq))
			continue;

		if (is_irq_blacklisted(irq))
			continue;

		cpumask_clear(&new_mask);
		cpumask_set_cpu(to, &new_mask);

		if (irq_set_affinity(irq, &new_mask) == 0)
			pr_info("ExoticBalance: Migrated IRQ %d from CPU%d to CPU%d\n", irq, from, to);
	}
}

static void exotic_balance_irq(struct work_struct *work)
{
	int irq, cpu;
	unsigned int max_irq = 0, min_irq = UINT_MAX;
	int max_cpu = -1, min_cpu = -1;
	int delta_sum = 0, delta_avg = 0;
	int dynamic_threshold;

	if (!exoticbalance_enabled)
		goto schedule_next;

	memset(cpu_irq_count, 0, sizeof(unsigned int) * nr_cpu_ids);

	for (irq = 0; irq < nr_irqs; irq++) {
		for_each_online_cpu(cpu)
			cpu_irq_count[cpu] += kstat_irqs_cpu(irq, cpu);
	}

	for_each_online_cpu(cpu) {
		unsigned int delta = cpu_irq_count[cpu] - cpu_irq_last[cpu];
		delta_sum += delta;

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

	delta_avg = delta_sum / num_online_cpus();
	dynamic_threshold = delta_avg + MIN_DELTA_IRQS_BASE;

	if (max_cpu >= 0 && min_cpu >= 0 && max_cpu != min_cpu) {
		if ((max_irq - min_irq) >= dynamic_threshold) {
			if (!is_cpu_big(min_cpu) && is_cpu_big(max_cpu)) {
				// Skip: avoid migrating to little core
			} else if (get_max_cpu_temp() >= MAX_CPU_TEMP_THRESHOLD) {
				// Skip: temperature too high
			} else {
				pr_info("ExoticBalance: Triggered migration from CPU%d (%u IRQs) to CPU%d (%u IRQs)\n",
				        max_cpu, max_irq, min_cpu, min_irq);
				migrate_irqs_simple(max_cpu, min_cpu);
			}
		}
	}

schedule_next:
	schedule_delayed_work(&balance_work, msecs_to_jiffies(BALANCE_INTERVAL_MS));
}

static int __init exoticbalance_init(void)
{
	cpu_irq_count = kzalloc(sizeof(unsigned int) * nr_cpu_ids, GFP_KERNEL);
	cpu_irq_last = kzalloc(sizeof(unsigned int) * nr_cpu_ids, GFP_KERNEL);
	if (!cpu_irq_count || !cpu_irq_last)
		return -ENOMEM;

	INIT_DELAYED_WORK(&balance_work, exotic_balance_irq);
	schedule_delayed_work(&balance_work, msecs_to_jiffies(BALANCE_INTERVAL_MS));

	pr_info("ExoticBalance: Initialized\n");
	return 0;
}

static void __exit exoticbalance_exit(void)
{
	cancel_delayed_work_sync(&balance_work);
	kfree(cpu_irq_count);
	kfree(cpu_irq_last);
	pr_info("ExoticBalance: Unloaded\n");
}

module_init(exoticbalance_init);
module_exit(exoticbalance_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobrut Exotic");
MODULE_DESCRIPTION("ExoticBalance: Smart IRQ load balancer with adaptive logic and critical IRQ protection");