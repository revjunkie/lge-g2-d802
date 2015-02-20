/* Copyright (c) 2015, Raj Ibrahim <rajibrahim@rocketmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

struct rev_tune
{
unsigned int shift_all;
unsigned int shift_cpu1;
unsigned int shift_threshold;
unsigned int down_shift;
unsigned int downshift_threshold;
unsigned int touchplug_duration;
unsigned int sample_time;
unsigned int min_cpu;
unsigned int max_cpu;
unsigned int down_diff;
unsigned int shift_diff;
unsigned int shift_diff_all;
} rev = {
	.shift_all = 185,
	.shift_cpu1 = 30,
	.shift_threshold = 4,
	.down_shift = 20,
	.downshift_threshold = 10,
	.sample_time = 100,
	.touchplug_duration = 5000,
	.min_cpu = 1,
	.max_cpu = 4,	
};

struct cpu_info
{
unsigned int cur;
};

static DEFINE_PER_CPU(struct cpu_info, rev_info);

static bool touchplug = true;
module_param(touchplug, bool, 0644);
static unsigned int debug = 0;
module_param(debug, uint, 0644);

#define dprintk(msg...)		\
do { 				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

static struct delayed_work hotplug_decision_work;
static struct work_struct touchplug_boost_work;
static struct delayed_work touchplug_down;
static struct workqueue_struct *hotplug_decision_wq;
static struct workqueue_struct *touchplug_wq;

extern unsigned int report_load_at_max_freq(void);

static inline void hotplug_all(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) 
		if (!cpu_online(cpu) && num_online_cpus() < rev.max_cpu) 
			cpu_up(cpu);
	
	rev.down_diff = 0;
	rev.shift_diff = 0;
}

static inline void hotplug_one(void)
{
	unsigned int cpu;
	
	cpu = cpumask_next_zero(0, cpu_online_mask);
		if (cpu < nr_cpu_ids)
			cpu_up(cpu);		
			dprintk("online CPU %d\n", cpu);
			
	rev.down_diff = 0;
	rev.shift_diff = 0;
}

static int get_idle_cpu(void)
{
	int i, cpu = 0;
	unsigned long i_state = 0;
	struct cpu_info *idle_info;
	
	for (i = 1; i < rev.max_cpu; i++) {
		if (!cpu_online(i))
			continue;
			idle_info = &per_cpu(rev_info, i);
			idle_info->cur = idle_cpu(i);
			dprintk("cpu %u idle state %d\n", i, idle_info->cur);
			if (i_state == 0) {
				cpu = i;
				i_state = idle_info->cur;
				continue;
			}	
			if (idle_info->cur > i_state) {
				cpu = i;
				i_state = idle_info->cur;
		}
	}
	return cpu;
}

static inline void unplug_one(void)
{	
	int cpu = get_idle_cpu();
	
	if (cpu < nr_cpu_ids) 
		cpu_down(cpu);
		dprintk("offline cpu %d\n", cpu);
		
	rev.down_diff = 0;		
	rev.shift_diff = 0;
	rev.shift_diff_all = 0;
}

static void __cpuinit touchplug_boost_work_fn(struct work_struct *work)
{
	unsigned int online_cpus = num_online_cpus();

	if (online_cpus == 1) 
 			cpu_up(1);	
	dprintk("touchplug detected\n");
}

static void  __cpuinit touchplug_down_fn(struct work_struct *work)
{
	unsigned int cpu, online_cpus = num_online_cpus();

	if (online_cpus == 2) {
		for_each_online_cpu(cpu)
			if (cpu_online(cpu))
			cpu_down(cpu);
	}
}

static void  __cpuinit hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus, down_load, up_load, load, down_shift;
	
	load = report_load_at_max_freq();
		dprintk("load is %d\n", load);
	online_cpus = num_online_cpus();
	up_load = min((rev.shift_cpu1 * online_cpus * online_cpus), rev.shift_all);
	down_shift = rev.shift_cpu1 * (online_cpus - 1) * (online_cpus - 1);
	down_load = min((down_shift - rev.down_shift), (rev.shift_all - rev.down_shift));

		if (load > rev.shift_all && online_cpus < rev.max_cpu && rev.shift_diff_all < (rev.shift_threshold - 2)) {
				rev.shift_diff_all++;
				dprintk("shift_diff_all is %d\n", rev.shift_diff_all);
			if (rev.shift_diff_all >= (rev.shift_threshold - 2)) {		
				hotplug_all();
				dprintk("revshift: Onlining all CPUs, load: %d\n", load);	
				}		
			}
		if (load <= rev.shift_all && online_cpus < rev.max_cpu && rev.shift_diff_all > 0) {
				rev.shift_diff_all = 0;
				dprintk("shift_diff_all reset to %d\n", rev.shift_diff_all);
				}	
		if (load > up_load && online_cpus < (rev.max_cpu - 1) && rev.shift_diff < rev.shift_threshold) {
				rev.shift_diff++;
				dprintk("shift_diff is %d\n", rev.shift_diff);
			if (rev.shift_diff >= rev.shift_threshold) {
				hotplug_one();	
				}				
			}	
		if (load <= up_load && online_cpus < (rev.max_cpu - 1) && rev.shift_diff > 0) {
				rev.shift_diff = 0;
				dprintk("shift_diff reset to %d\n", rev.shift_diff);
			}
		if (load < down_load && online_cpus > rev.min_cpu && rev.down_diff < rev.downshift_threshold) {
				dprintk("down_load is %d\n", down_load);	
				rev.down_diff++;
				dprintk("down_diff is %d\n", rev.down_diff);
			if (rev.down_diff >= rev.downshift_threshold) {
				if (touchplug) {
					if (online_cpus == 2) {
						schedule_delayed_work_on(0, &touchplug_down, msecs_to_jiffies(rev.touchplug_duration));
					} else 
						unplug_one();
					} else
					unplug_one();
				}
			}	
		if (load >= down_load && online_cpus > rev.min_cpu && rev.down_diff > 0) {	
				rev.down_diff--;
				dprintk("down_diff reset to %d\n", rev.down_diff);
			}		
	queue_delayed_work(hotplug_decision_wq, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time));
}

/**************SYSFS*******************/

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct device * dev, struct device_attribute * attr, char * buf)	\
{									\
	return sprintf(buf, "%u\n", rev.object);			\
}
show_one(shift_cpu1, shift_cpu1);
show_one(shift_all, shift_all);
show_one(shift_threshold, shift_threshold);
show_one(down_shift, down_shift);
show_one(downshift_threshold, downshift_threshold);
show_one(sample_time, sample_time);
show_one(touchplug_duration, touchplug_duration);
show_one(min_cpu,min_cpu);
show_one(max_cpu,max_cpu);


#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct device * dev, struct device_attribute * attr, const char * buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	rev.object = input;						\
	return count;							\
}			
store_one(shift_cpu1, shift_cpu1);
store_one(shift_all, shift_all);
store_one(shift_threshold, shift_threshold);
store_one(down_shift, down_shift);
store_one(downshift_threshold, downshift_threshold);
store_one(sample_time, sample_time);
store_one(touchplug_duration, touchplug_duration);
store_one(min_cpu,min_cpu);
store_one(max_cpu,max_cpu);

static DEVICE_ATTR(shift_cpu1, 0644, show_shift_cpu1, store_shift_cpu1);
static DEVICE_ATTR(shift_all, 0644, show_shift_all, store_shift_all);
static DEVICE_ATTR(shift_threshold, 0644, show_shift_threshold, store_shift_threshold);
static DEVICE_ATTR(down_shift, 0644, show_down_shift, store_down_shift);
static DEVICE_ATTR(downshift_threshold, 0644, show_downshift_threshold, store_downshift_threshold);
static DEVICE_ATTR(sample_time, 0644, show_sample_time, store_sample_time);
static DEVICE_ATTR(touchplug_duration, 0644, show_touchplug_duration, store_touchplug_duration);
static DEVICE_ATTR(min_cpu, 0644, show_min_cpu, store_min_cpu);
static DEVICE_ATTR(max_cpu, 0644, show_max_cpu, store_max_cpu);

static struct attribute *revshift_hotplug_attributes[] = 
    {
	&dev_attr_shift_cpu1.attr,
	&dev_attr_shift_all.attr,
	&dev_attr_shift_threshold.attr,
	&dev_attr_down_shift.attr,
	&dev_attr_downshift_threshold.attr,
	&dev_attr_sample_time.attr,
	&dev_attr_touchplug_duration.attr,
	&dev_attr_min_cpu.attr,
	&dev_attr_max_cpu.attr,
	NULL
    };

static struct attribute_group revshift_hotplug_group = 
    {
	.attrs  = revshift_hotplug_attributes,
    };

static struct miscdevice revshift_hotplug_device = 
    {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "revshift_hotplug",
    };


static void touchplug_input_event(struct input_handle *handle,
 		unsigned int type, unsigned int code, int value)
{
 	if (touchplug) {	
	queue_work(touchplug_wq, &touchplug_boost_work);
	}
}
 
static int touchplug_input_connect(struct input_handler *handler,
 		struct input_dev *dev, const struct input_device_id *id)
{
 	struct input_handle *handle;
 	int error;
 
 	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
 	if (!handle)
 		return -ENOMEM;
 
 	handle->dev = dev;
 	handle->handler = handler;
 	handle->name = "touchplug_input_handler";

 
 	error = input_register_handle(handle);
 	if (error)
 		goto err2;
 
 	error = input_open_device(handle);
 	if (error)
 		goto err1;
 	dprintk("%s found and connected!\n", dev->name);
 	return 0;
 err1:
 	input_unregister_handle(handle);
 err2:
 	kfree(handle);
 	return error;
}
 
static void touchplug_input_disconnect(struct input_handle *handle)
{
 	input_close_device(handle);
 	input_unregister_handle(handle);
 	kfree(handle);
}

static const struct input_device_id touchplug_ids[] = {
 	{
 		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
 			 INPUT_DEVICE_ID_MATCH_ABSBIT,
 		.evbit = { BIT_MASK(EV_ABS) },
 		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
 			    BIT_MASK(ABS_MT_POSITION_X) |
 			    BIT_MASK(ABS_MT_POSITION_Y) },
 	}, /* multi-touch touchscreen */
 	{
 		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
 			 INPUT_DEVICE_ID_MATCH_ABSBIT,
 		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
 		.absbit = { [BIT_WORD(ABS_X)] =
 			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
 	}, /* touchpad */
 	{ },
};
 
static struct input_handler touchplug_input_handler = {
 	.event          = touchplug_input_event,
 	.connect        = touchplug_input_connect,
 	.disconnect     = touchplug_input_disconnect,
 	.name           = "touchplug_input_handler",
 	.id_table       = touchplug_ids,
};

int __init revshift_hotplug_init(void)
{
	int ret;

	ret = input_register_handler(&touchplug_input_handler);
	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	ret = misc_register(&revshift_hotplug_device);
	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	ret = sysfs_create_group(&revshift_hotplug_device.this_device->kobj,
			&revshift_hotplug_group);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	hotplug_decision_wq = alloc_workqueue("hotplug_decision_work",
				WQ_HIGHPRI | WQ_UNBOUND, 0);
	touchplug_wq = alloc_workqueue("touchplug", WQ_HIGHPRI, 0);	

	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);
	INIT_DELAYED_WORK(&touchplug_down, touchplug_down_fn);
	INIT_WORK(&touchplug_boost_work, touchplug_boost_work_fn);

	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 20);
	return 0;
	
err:
	return ret;
}
late_initcall(revshift_hotplug_init);

