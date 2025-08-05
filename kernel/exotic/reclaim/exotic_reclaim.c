// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/timekeeping.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/sysctl.h>

#define RECLAIM_INTERVAL (60 * HZ)      // check every 1 min
#define SCREEN_OFF_DELAY (10 * 60)      // 10 min

extern int sysctl_drop_caches;

static struct delayed_work reclaim_work;
static unsigned long screen_off_jiffies = 0;
static bool screen_is_off = false;

static void do_reclaim(struct work_struct *work)
{
	unsigned long uptime_sec = ktime_get_boottime_seconds();
	unsigned long now = jiffies;

	if (uptime_sec < 10800) // less than 3 hours
		goto reschedule;

	if (!screen_is_off || !time_after(now, screen_off_jiffies + SCREEN_OFF_DELAY * HZ))
		goto reschedule;

	pr_info("ExoticReclaim: conditions met, dropping caches\n");

	// drop pagecache + dentries + inodes
	sysctl_drop_caches = 3;

reschedule:
	schedule_delayed_work(&reclaim_work, RECLAIM_INTERVAL);
}

static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct fb_event *evdata = data;

	if (event == FB_EVENT_BLANK && evdata && evdata->data) {
		int *blank = evdata->data;
		if (*blank == FB_BLANK_POWERDOWN) {
			screen_is_off = true;
			screen_off_jiffies = jiffies;
		} else {
			screen_is_off = false;
		}
	}
	return 0;
}

static struct notifier_block fb_nb = {
	.notifier_call = fb_notifier_callback,
};

static int __init exotic_reclaim_init(void)
{
	pr_info("ExoticReclaim: loaded\n");
	INIT_DELAYED_WORK(&reclaim_work, do_reclaim);
	schedule_delayed_work(&reclaim_work, RECLAIM_INTERVAL);
	fb_register_client(&fb_nb);
	return 0;
}

static void __exit exotic_reclaim_exit(void)
{
	pr_info("ExoticReclaim: exiting\n");
	cancel_delayed_work_sync(&reclaim_work);
	fb_unregister_client(&fb_nb);
}

module_init(exotic_reclaim_init);
module_exit(exotic_reclaim_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ExoticReclaim - Auto cache cleaner after 3h + screen off 10m");
MODULE_AUTHOR("Tobrut Kernel");