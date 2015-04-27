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

#ifndef _LINUX_VMPRESSURE_H
#define _LINUX_VMPRESSURE_H

#include <linux/types.h>

/**
 * enum vmpressure_level - Memory pressure levels
 *  <at> VMPRESSURE_LOW:	The system is short on idle pages, losing caches
 *  <at> VMPRESSURE_MEDIUM:	New allocations' cost becomes high
 *  <at> VMPRESSURE_OOM:	The system is about to go out-of-memory
 */
enum vmpressure_level {
	/* We spread the values, reserving room for new levels. */
	VMPRESSURE_LOW		= 1 << 10,
	VMPRESSURE_MEDIUM	= 1 << 20,
	VMPRESSURE_OOM		= 1 << 30,
};

/**
 * struct vmpressure_config - Configuration structure for vmpressure_fd()
 *  <at> size:	Size of the struct for ABI extensibility
 *  <at> threshold:	Minimum pressure level of notifications
 *
 * This structure is used to configure the file descriptor that
 * vmpressure_fd() returns.
 *
 *  <at> size is used to "version" the ABI, it must be initialized to
 * 'sizeof(struct vmpressure_config)'.
 *
 *  <at> threshold should be one of  <at> vmpressure_level values, and specifies
 * minimal level of notification that will be delivered.
 */
struct vmpressure_config {
	__u32 size;
	__u32 threshold;
};

/**
 * struct vmpressure_event - An event that is returned via vmpressure fd
 *  <at> pressure:	Most recent system's pressure level
 *
 * Upon notification, this structure must be read from the vmpressure file
 * descriptor.
 */
struct vmpressure_event {
	__u32 pressure;
};

#ifdef __KERNEL__

struct mem_cgroup;

#ifdef CONFIG_VMPRESSURE

extern uint vmpressure_win;
extern uint vmpressure_level_med;
extern uint vmpressure_level_oom;
extern uint vmpressure_level_oom_prio;

extern void __vmpressure(struct mem_cgroup *memcg,
			 ulong scanned, ulong reclaimed);
static void vmpressure(struct mem_cgroup *memcg,
		       ulong scanned, ulong reclaimed);

/*
 * OK, we're cheating. The thing is, we have to average s/r ratio by
 * gathering a lot of scans (otherwise we might get some local
 * false-positives index of '100').
 *
 * But... when we're almost OOM we might be getting the last reclaimable
 * pages slowly, scanning all the queues, and so we never catch the OOM
 * case via averaging. Although the priority will show it for sure. The
 * pre-OOM priority value is mostly an empirically taken priority: we
 * never observe it under any load, except for last few allocations before
 * the OOM (but the exact value is still configurable via sysctl).
 */
static inline void vmpressure_prio(struct mem_cgroup *memcg, int prio)
{
	if (prio > vmpressure_level_oom_prio)
		return;

	/* OK, the prio is below the threshold, send the pre-OOM event. */
	vmpressure(memcg, vmpressure_win, 0);
}

#else
static inline void __vmpressure(struct mem_cgroup *memcg,
				ulong scanned, ulong reclaimed) {}
static inline void vmpressure_prio(struct mem_cgroup *memcg, int prio) {}
#endif /* CONFIG_VMPRESSURE */

static inline void vmpressure(struct mem_cgroup *memcg,
			      ulong scanned, ulong reclaimed)
{
	if (!scanned)
		return;

	if (IS_BUILTIN(CONFIG_MEMCG) && memcg) {
		/*
		 * The vmpressure API reports system pressure, for per-cgroup
		 * pressure, we'll chain cgroups notifications, this is to
		 * be implemented.
		 *
		 * memcg_vm_pressure(target_mem_cgroup, scanned, reclaimed);
		 */
		return;
	}
	__vmpressure(memcg, scanned, reclaimed);
}

#endif /* __KERNEL__ */

#endif /* _LINUX_VMPRESSURE_H */
