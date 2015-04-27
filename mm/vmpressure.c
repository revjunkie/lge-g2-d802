/*
 * Linux VM pressure notifications
 *
 * Copyright 2011-2012 Pekka Enberg <penberg <at> kernel.org>
 * Copyright 2011-2012 Linaro Ltd.
 *		       Anton Vorontsov <anton.vorontsov <at> linaro.org>
 *
 * Based on ideas from KOSAKI Motohiro, Leonid Moiseichuk, Mel Gorman,
 * Minchan Kim and Pekka Enberg.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/vmpressure.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/swap.h>

struct vmpressure_watch {
	struct vmpressure_config config;
	atomic_t pending;
	wait_queue_head_t waitq;
	struct list_head node;
};

static atomic64_t vmpressure_sr;
static uint vmpressure_val;

static LIST_HEAD(vmpressure_watchers);
static DEFINE_MUTEX(vmpressure_watchers_lock);

/* Our sysctl tunables, see Documentation/sysctl/vm.txt */
uint __read_mostly vmpressure_win = SWAP_CLUSTER_MAX * 16;
uint vmpressure_level_med = 60;
uint vmpressure_level_oom = 99;
uint vmpressure_level_oom_prio = 4;

/*
 * This function is called from a workqueue, which can have only one
 * execution thread, so we don't need to worry about racing w/ ourselves.
 * And so it possible to implement the lock-free logic, using just the
 * atomic watch->pending variable.
 */
static void vmpressure_sample(struct vmpressure_watch *watch)
{
	if (atomic_read(&watch->pending))
		return;
	if (vmpressure_val < watch->config.threshold)
		return;

	atomic_set(&watch->pending, 1);
	wake_up(&watch->waitq);
}

static u64 vmpressure_level(uint pressure)
{
	if (pressure >= vmpressure_level_oom)
		return VMPRESSURE_OOM;
	else if (pressure >= vmpressure_level_med)
		return VMPRESSURE_MEDIUM;
	return VMPRESSURE_LOW;
}

static uint vmpressure_calc_pressure(uint win, uint s, uint r)
{
	ulong p;

	/*
	 * We calculate the ratio (in percents) of how many pages were
	 * scanned vs. reclaimed in a given time frame (window). Note that
	 * time is in VM reclaimer's "ticks", i.e. number of pages
	 * scanned. This makes it possible set desired reaction time and
	 * serves as a ratelimit.
	 */
	p = win - (r * win / s);
	p = p * 100 / win;

	pr_debug("%s: %3lu  (s: %6u  r: %6u)\n", __func__, p, s, r);

	return vmpressure_level(p);
}

#define VMPRESSURE_SCANNED_SHIFT (sizeof(u32) * 8 / 2)

static void vmpressure_wk_fn(struct work_struct *wk)
{
	struct vmpressure_watch *watch;
	u64 sr = atomic64_xchg(&vmpressure_sr, 0);
	u32 s = sr >> VMPRESSURE_SCANNED_SHIFT;
	u32 r = sr & (((u64)1 << VMPRESSURE_SCANNED_SHIFT) - 1);

	vmpressure_val = vmpressure_calc_pressure(vmpressure_win, s, r);

	mutex_lock(&vmpressure_watchers_lock);
	list_for_each_entry(watch, &vmpressure_watchers, node)
		vmpressure_sample(watch);
	mutex_unlock(&vmpressure_watchers_lock);
}
static DECLARE_WORK(vmpressure_wk, vmpressure_wk_fn);

void __vmpressure(struct mem_cgroup *memcg, ulong scanned, ulong reclaimed)
{
	/*
	 * Store s/r combined, so we don't have to worry to synchronize
	 * them. On modern machines it will be truly atomic; on arches w/o
	 * 64 bit atomics it will turn into a spinlock (for a small amount
	 * of CPUs it's not a problem).
	 *
	 * Using int-sized atomics is a bad idea as it would only allow to
	 * count (1 << 16) - 1 pages (256MB), which we can scan pretty
	 * fast.
	 *
	 * We can't have per-CPU counters as this will not catch a case
	 * when many CPUs scan small amounts (so none of them hit the
	 * window size limit, and thus we won't send a notification in
	 * time).
	 *
	 * So we shouldn't place vmpressure() into a very hot path.
	 */
	atomic64_add(scanned << VMPRESSURE_SCANNED_SHIFT | reclaimed,
		     &vmpressure_sr);

	scanned = atomic64_read(&vmpressure_sr) >> VMPRESSURE_SCANNED_SHIFT;
	if (scanned >= vmpressure_win && !work_pending(&vmpressure_wk))
		schedule_work(&vmpressure_wk);
}

static uint vmpressure_poll(struct file *file, poll_table *wait)
{
	struct vmpressure_watch *watch = file->private_data;

	poll_wait(file, &watch->waitq, wait);

	return atomic_read(&watch->pending) ? POLLIN : 0;
}

static ssize_t vmpressure_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct vmpressure_watch *watch = file->private_data;
	struct vmpressure_event event;
	int ret;

	if (count < sizeof(event))
		return -EINVAL;

	ret = wait_event_interruptible(watch->waitq,
				       atomic_read(&watch->pending));
	if (ret)
		return ret;

	event.pressure = vmpressure_val;
	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	atomic_set(&watch->pending, 0);

	return count;
}

static int vmpressure_release(struct inode *inode, struct file *file)
{
	struct vmpressure_watch *watch = file->private_data;

	mutex_lock(&vmpressure_watchers_lock);
	list_del(&watch->node);
	mutex_unlock(&vmpressure_watchers_lock);

	kfree(watch);
	return 0;
}

static const struct file_operations vmpressure_fops = {
	.poll		= vmpressure_poll,
	.read		= vmpressure_read,
	.release	= vmpressure_release,
};

SYSCALL_DEFINE1(vmpressure_fd, struct vmpressure_config __user *, config)
{
	struct vmpressure_watch *watch;
	struct file *file;
	int ret;
	int fd;

	watch = kzalloc(sizeof(*watch), GFP_KERNEL);
	if (!watch)
		return -ENOMEM;

	ret = copy_from_user(&watch->config, config, sizeof(*config));
	if (ret)
		goto err_free;

	fd = get_unused_fd_flags(O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto err_free;
	}

	file = anon_inode_getfile("[vmpressure]", &vmpressure_fops, watch,
				  O_RDONLY);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_fd;
	}

	fd_install(fd, file);

	init_waitqueue_head(&watch->waitq);

	mutex_lock(&vmpressure_watchers_lock);
	list_add(&watch->node, &vmpressure_watchers);
	mutex_unlock(&vmpressure_watchers_lock);

	return fd;
err_fd:
	put_unused_fd(fd);
err_free:
	kfree(watch);
	return ret;
}
