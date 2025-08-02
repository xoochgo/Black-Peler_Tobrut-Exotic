// SPDX-License-Identifier: GPL-2.0
/*
 * ExoticReclaim - Automatic RAM cleaner every 3h after boot, only when screen off >10 min
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/timekeeping.h>

#define RECLAIM_INTERVAL_SECS (3 * 3600) 
#define SCREEN_OFF_REQUIRED_SECS (10 * 60) 

static struct delayed_work exotic_reclaim_work;
static ktime_t last_reclaim_time;
static ktime_t screen_off_time;
static bool screen_is_off = false;

static void exotic_reclaim_fn(struct work_struct *work)
{
	ktime_t now = ktime_get_boottime_seconds();

	if (screen_is_off &&
	    ktime_to_secs(ktime_sub(now, screen_off_time)) >= SCREEN_OFF_REQUIRED_SECS &&
	    ktime_to_secs(ktime_sub(now, last_reclaim_time)) >= RECLAIM_INTERVAL_SECS) {

		pr_info("ExoticReclaim: Dropping caches...\n");
		sysctl_drop_caches = 3;
		last_reclaim_time = now;
	}

	schedule_delayed_work(&exotic_reclaim_work, msecs_to_jiffies(60 * 1000)); 
}

static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = evdata->data;
	screen_is_off = (*blank != FB_BLANK_UNBLANK);
	if (screen_is_off)
		screen_off_time = ktime_get_boottime_seconds();

	pr_info("ExoticReclaim: screen_is_off = %d\n", screen_is_off);
	return NOTIFY_OK;
}

static struct notifier_block fb_notifier = {
	.notifier_call = fb_notifier_callback,
};

static int __init exotic_reclaim_init(void)
{
	pr_info("ExoticReclaim: Init\n");

	last_reclaim_time = ktime_get_boottime_seconds();
	INIT_DELAYED_WORK(&exotic_reclaim_work, exotic_reclaim_fn);
	schedule_delayed_work(&exotic_reclaim_work, msecs_to_jiffies(60 * 1000));

	fb_register_client(&fb_notifier);
	return 0;
}

static void __exit exotic_reclaim_exit(void)
{
	cancel_delayed_work_sync(&exotic_reclaim_work);
	fb_unregister_client(&fb_notifier);
	pr_info("ExoticReclaim: Exit\n");
}

module_init(exotic_reclaim_init);
module_exit(exotic_reclaim_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ExoticReclaim - Auto RAM cleaner");
MODULE_AUTHOR("Mr. Morat");