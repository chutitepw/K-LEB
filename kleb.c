/* Copyright (c) 2017, 2024 James Bruska, Caleb DeLaBruere, Chutitep Woralert

This file is part of K-LEB.

K-LEB is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

K-LEB is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with K-LEB.  If not, see <https://www.gnu.org/licenses/>. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>	// high res timer
#include <linux/ktime.h>	// ktime representation
#include <linux/math64.h>	// div_u64
#include <linux/slab.h>		// kmalloc
#include <linux/device.h>	// character devices
#include <linux/fs.h>		// file control
#include <linux/cdev.h>
#include <linux/version.h>	// linux version
#include <asm/uaccess.h>
#include <asm/nmi.h>		// reserve_perfctr_nmi ...
#include <asm/perf_event.h>	// union cpuid10...
#include <asm/special_insns.h> // read and write cr4
#include "kleb.h"

#include <linux/kprobes.h> 	// kprobe and jprobe
#include <linux/sched.h> 	//finish_task_switch

#include <linux/time.h>

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36)
#define UNLOCKED 1
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chutitep Woralert, James Bruska, Caleb DeLaBruere");
MODULE_DESCRIPTION("K-LEB: A hardware event recording system with a high resolution timer");
MODULE_VERSION("0.8.0");

/* Module parameters */
static struct hrtimer hr_timer;
static ktime_t ktime_period_ns;
static unsigned int delay_in_ns;
static int num_events, num_coreevents, num_l3events, counter, timer_restart;
static int target_pid, recording;
static unsigned int **hardware_events;
static int Major;
static kleb_ioctl_args_t kleb_ioctl_args;
static int sysmode;
#define NUM_CORES num_online_cpus()
#define num_recordings 500
/* For tapping */
struct cdev *kernel_cdev;

typedef struct target_id{
	int pid;
	int status;
    int on_cpu;
}target_id;

typedef struct {
	target_id *target_pid;
	size_t size;
    int index_size;
    int index;
	int new_targetid;
}target_array;

static target_array *target;

/* Counters parameters */
static int reg_addr, reg_addr_val, event_num, umask, enable_bits, disable_bits, event_on, event_off, l3enable_bits, l3disable_bits;
static int core_events[10];
static int l3_events[10];
static int addr[6], l3_addr[6];
static int addr_val[6], l3_addr_val[6];
unsigned long long counter_umask;
unsigned int user_os_rec;


/* Handle context switch & CPU switch */

static unsigned int hardware_events_core[16];

/* Initialize counters */
static long pmu_start_counters(void)
{
	int i = 0;

	/* Setup event selector  */
	addr[0] = 0xc0010200; 
	addr[1] = 0xc0010202;  
	addr[2] = 0xc0010204;
	addr[3] = 0xc0010206;
	addr[4] = 0xc0010208;
	addr[5] = 0xc001020A;

	/* Setup counter */
	addr_val[0] = 0xc0010201; 
	addr_val[1] = 0xc0010203;
	addr_val[2] = 0xc0010205;
	addr_val[3] = 0xc0010207;
	addr_val[4] = 0xc0010209;
	addr_val[5] = 0xc001020B;
	
	/* Setup l3 event selector  */
	l3_addr[0] = 0xc0010230;
	l3_addr[1] = 0xc0010232;
	l3_addr[2] = 0xc0010234;
	l3_addr[3] = 0xc0010236;
	l3_addr[4] = 0xc0010238;
	l3_addr[5] = 0xc001023A;

	/* Setup l3 counter */
	l3_addr_val[0] = 0xc0010231;
	l3_addr_val[1] = 0xc0010233;
	l3_addr_val[2] = 0xc0010235;
	l3_addr_val[3] = 0xc0010237;
	l3_addr_val[4] = 0xc0010239;
	l3_addr_val[5] = 0xc001023B;
	
	/* Seperate event type */
	num_coreevents = 0;
	num_l3events = 0;
	for (i = 0; i < num_events; ++i)
	{
		//printk_d(KERN_INFO "Events Input: %x\n", kleb_ioctl_args.counter[i]);

		/* Check L3 cache event */
		if(  kleb_ioctl_args.counter[i]==0xff9a || kleb_ioctl_args.counter[i]==0x0090 || kleb_ioctl_args.counter[i]==0x019A){
			l3_events[num_l3events] = kleb_ioctl_args.counter[i];
			//printk_d(KERN_INFO "l3Events convert: %x\n", l3_events[num_l3events]);
			++num_l3events;	
		}
		else{
			core_events[num_coreevents] = kleb_ioctl_args.counter[i];
			++num_coreevents;
		}
	}
	
	/* Define setup parameters */
	user_os_rec &= 0x01; // Enforces requirement of 0 <= user_os_rec <= 3
	enable_bits = (0x01 << 22) + (user_os_rec << 16);
	if(sysmode)
		l3enable_bits = ((uint64_t)0b11 << 56) + ((uint64_t)0x1 << 47) + ((uint64_t)0x1 << 46) + (0x01 << 22) + (user_os_rec << 16);
	else
		l3enable_bits = ((uint64_t)0b11 << 56) + ((uint64_t)0x1 << 47) + ((uint64_t)0x1 << 46) + (0x01 << 22) + (user_os_rec << 16);
	//printk( KERN_INFO "BIT: %llu %llu \n", enable_bits, l3enable_bits);
	disable_bits = (0x00 << 22) + (user_os_rec << 16);
	l3disable_bits = (0x00 << 22) + (user_os_rec << 16);
	counter_umask &= 0x00; // Enforces requirement of 0 <= counter_umask <= 0xFF
	umask = counter_umask << 8;

	memset(hardware_events_core, 0, sizeof(hardware_events_core));

	return 0;
}

/* Disable counting */
static long pmu_stop_counters(int current_core)
{
	int i;
	/* Disable configurable counters */
	for (i = 0; i < num_coreevents; i++)
	{
		//reg_addr_val = addr_val[i];
		reg_addr = addr[i];
		event_off = event_num | umask | disable_bits;

		/* Set event off */
		if(sysmode){
			wrmsrl_on_cpu(current_core, reg_addr, event_off);
		}
		else{
			__asm__("wrmsr"
			:
			: "c"(reg_addr), "a"(event_off), "d"(0x00));
		}
	}
	
	for (i = 0; i < num_l3events; i++)
	{
		//reg_addr_val = l3_addr_val[i];
		reg_addr = l3_addr[i];
		event_off = event_num | umask | l3disable_bits;
		if(sysmode){
			wrmsrl_on_cpu(current_core, reg_addr, 0x00);
		}
		else{
			__asm__("wrmsr"
			:
			: "c"(reg_addr), "a"(event_off), "d"(0x00));
		}
		
	}

	return 0;
}

/* Enable counting */
static long pmu_restart_counters(int current_core)
{
	int i = 0;

	/* Enable configuration counters */
	for (i = 0; i < num_coreevents; i++)
	{
		reg_addr_val = addr_val[i];
		reg_addr = addr[i];
		event_num = core_events[i];
		event_on = event_num | umask | l3enable_bits;

		if(sysmode){
			/* Clear old value */
			wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);		

			/* Enable counting */
			wrmsrl_on_cpu(current_core, reg_addr, event_on);
		}
		else{
			__asm__("wrmsr"
			:
			: "c"(reg_addr_val), "a"(0x0), "d"(0x0));
			__asm__("wrmsr"
			:
			: "c"(reg_addr), "a"(event_on), "d"(0x00));
		}

	}

	for (i = 0; i < num_l3events; i++)
	{
		reg_addr_val = l3_addr_val[i];
		reg_addr = l3_addr[i];
		event_num = l3_events[i];
		if(sysmode){
			event_on = event_num | umask | l3enable_bits;

			/* Reset counter value */
			wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);

			/* Enable counting */
			wrmsrl_on_cpu(current_core, reg_addr, event_on);
		}
		else{
			event_on = event_num | umask | (l3enable_bits + ((uint64_t)0x0 << 42) + ((uint64_t)0x0 << 48));
			__asm__("wrmsr"
			:
			: "c"(reg_addr_val), "a"(0x0), "d"(0x0));
			__asm__("wrmsr"
			:
			: "c"(reg_addr), "a"(event_on), "d"(0x00));
		}	

	}
	
	return 1;
}

/* Read counters value */
static long pmu_read_counters(void)
{
	int i;
	/* Read configuration counters */
	for ( i = 0; i < num_events; i++)
	{
		/******* Counting/Subtract ********/
		hardware_events[i][counter] = hardware_events_core[i];
		hardware_events_core[i] = 0;
	}

	return 0;
}
static u64 pmu_read_counters_core(int current_core)
{
	int i = 0;
	u64 val;
	
	/* Read configuration counters */
	for (i = 0; i < num_coreevents; i++)
	{
		reg_addr_val = addr_val[i];

		/* Read & reset counter value */
		if(sysmode){
			rdmsrl_on_cpu(current_core, reg_addr_val, &val);
			wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);
		}
		else{
			__asm__("rdmsr"
					: "=A"(val)
					: "c"(reg_addr_val));

			__asm__("wrmsr"
			:
			: "c"(reg_addr_val), "a"(0x00), "d"(0x00));
		}

		hardware_events_core[i] += val;
	}

	/* Read l3 counters */
	for (i = 0; i < num_l3events; i++)
	{
		reg_addr_val = l3_addr_val[i];
		if(sysmode){
			rdmsrl_on_cpu(current_core, reg_addr_val, &val);
			wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);
		}
		else{
			__asm__("rdmsr"
					: "=A"(val)
					: "c"(reg_addr_val));

			__asm__("wrmsr"
			:
			: "c"(reg_addr_val), "a"(0x00), "d"(0x00));
		}

		hardware_events_core[i+num_coreevents] += val;
	}

	return 0;
}
static u64 pmu_read_counters_core_oncpu(int current_core)
{
	int i = 0;
	u64 val;
	
	/* Read configuration counters */
	for (i = 0; i < num_coreevents; i++)
	{
		reg_addr_val = addr_val[i];

		/* Read & reset counter value */
		rdmsrl_on_cpu(current_core, reg_addr_val, &val);
		wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);

		hardware_events_core[i] += val;
	}

	/* Read l3 counters */
	for (i = 0; i < num_l3events; i++)
	{
		reg_addr_val = l3_addr_val[i];

		rdmsrl_on_cpu(current_core, reg_addr_val, &val);
		wrmsrl_on_cpu(current_core, reg_addr_val, 0x0);


		hardware_events_core[i+num_coreevents] += val;
	}
	//printk(KERN_INFO "Count Read Core %u\n", hardware_events_core[0]);
	return 0;
}
int kprobes_handle_finish_task_switch_pre(struct kprobe *p, struct pt_regs *regs)
{
	if(recording && (counter < num_recordings))
	{
		//printk(KERN_INFO "Monitor start on CPU: %d", current->on_cpu);
		int i, j;
		if(!sysmode){
			for(i=0; i < target->index_size; ++i){
				if (current->pid == target->target_pid[i].pid && current->pid != 0 && current->pid != 1){
					target->target_pid[i].on_cpu = current->on_cpu;
					target->target_pid[i].status = 1;
					//Call start
					//printk(KERN_INFO "task_switch IN %d %d %d %d %d %d\n",current->pid, target->target_pid[i].pid, current->parent->pid, current->tgid, current->on_cpu, target->index_size);
					pmu_restart_counters(target->target_pid[i].on_cpu);
					break;
				}
				else if(current->parent->pid == target->target_pid[i].pid && current->parent->pid != 0){
					target->new_targetid = 1;
					/* Check if fork already exist */
					for(j=0; j < target->index_size; ++j){
						if(current->pid == target->target_pid[j].pid){
							target->new_targetid = 0;
							break;
						}
					}
					if(target->new_targetid){
						target->index_size += 1;
						if(target->index_size >= target->size){
							target->size = target->size+100;
							target->target_pid = krealloc(target->target_pid,sizeof(target_id)*target->size, GFP_KERNEL); 
						}
						target->target_pid[target->index_size].pid = current->pid;
						target->target_pid[target->index_size].on_cpu = current->on_cpu;
						target->target_pid[target->index_size].status = 1;
					}		
				}
				else if(current->tgid == target->target_pid[i].pid && current->tgid != 0){
					target->new_targetid = 1;
					/* Check if thread already exist */
					for(j=0; j < target->index_size; ++j){
						if(current->pid == target->target_pid[j].pid){
							target->new_targetid = 0;
							break;
						}
					}
					if(target->new_targetid){
						target->index_size += 1;
						if(target->index_size >= target->size){
							target->size = target->size+100;
							target->target_pid = krealloc(target->target_pid,sizeof(target_id)*target->size, GFP_KERNEL); 
						}
						target->target_pid[target->index_size].pid = current->pid;
						target->target_pid[target->index_size].on_cpu = current->on_cpu;
						target->target_pid[target->index_size].status = 1;
					}	
				}
				else{
					if(current->pid != target->target_pid[i].pid && target->target_pid[i].status == 1 && current->on_cpu == target->target_pid[i].on_cpu){
						target->target_pid[i].on_cpu = -1;
						target->target_pid[i].status = 0;
						//Call stop
						pmu_read_counters_core(current->on_cpu);
						pmu_stop_counters(current->on_cpu);

					}
				}
			}
		}
	}

	return 0;
}
/* void kprobes_handle_finish_task_switch_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	if(recording && (counter < num_recordings))
	{
			if (current->pid == target_pid)
			{
				printk(KERN_INFO "finish_task_switch_post: %s [%d] [%d] [%d]\n", current->comm, current->pid, current->on_cpu, current->state);
			}
		
	}
}*/
static int kprobes_handle_do_exit_pre(struct kprobe *p, struct pt_regs *regs)
{
	if(recording && (counter < num_recordings) && !sysmode)
	{
		int i, j;
		for(i=0; i < target->index_size; ++i){
			if(current->pid == target->target_pid[i].pid && current->pid != 0 && current->pid != 1){
				
				/* Extract last data */ 
				if(counter < num_recordings){
					pmu_read_counters_core_oncpu(target->target_pid[i].on_cpu);
				}
				//Call stop
				pmu_stop_counters(target->target_pid[i].on_cpu);
				
				/* Remove and shift array elements */
				for (j = i; j < target->index_size-1; j++){
					target->target_pid[j].pid = target->target_pid[j+1].pid;
					target->target_pid[j].status = target->target_pid[j+1].status;
					target->target_pid[j].on_cpu = target->target_pid[j+1].on_cpu;
				}
				target->index_size -= 1;

				/* Remove excess space if necessary */
				if(target->index_size < target->size-100 && target->size > 100){
					target->size = target->size-100;
					target->target_pid = krealloc(target->target_pid,sizeof(target_id)*target->size, GFP_KERNEL); 
				}

				//printk(KERN_INFO "Task (do_exit): %d %d %d %d %d %d\n", current->pid, target->target_pid[i].pid,current->parent->pid,current->tgid, current->on_cpu, target->index_size);
				break;
			}
		}
	}
	return 0;
}
/*void kprobes_handle_do_exit_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	if(recording && (counter < num_recordings))
	{
		for(int i=0; i < target->index_size; ++i){
			if(current->pid == target->target_pid->pid && current->pid != 0 && current->pid != 1){
				printk(KERN_INFO "Task (do_exit_post): %d %d ID %d %d %d\n", current->exit_code, current->exit_state, current->pid, current->parent->pid,current->tgid);
			}
		}
		
	}
}*/
/*static int kprobes_handle_do_fork_pre(struct kprobe *p, struct pt_regs *regs)
{
	if(recording && (counter < num_recordings))
	{
		for(int i=0; i < target->index_size; ++i)
		{
			if (current->parent->pid == target->target_pid[i].pid)
			{
				printk(KERN_INFO "do_fork_pre: %s [%d] [%d] [%ld]\n", current->comm, current->pid, current->parent->pid, current->tgid);
			}
		}
	}
	return 0;
}
static void kprobes_handle_do_fork_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	if(recording && (counter < num_recordings))
	{
		for(int i=0; i < target->index_size; ++i)
		{
			if (current->parent->pid == target->target_pid[i].pid)
			{
				printk(KERN_INFO "do_fork_post: %s [%d] [%d] [%ld]\n", current->comm, current->pid, current->parent->pid, current->tgid);
			}
		}
	}
}*/
static struct kprobe finish_task_switch_kp = {
	.pre_handler = kprobes_handle_finish_task_switch_pre,
	//.post_handler = kprobes_handle_finish_task_switch_post,
	.symbol_name = DO_EXIT_NAME,
};
 static struct kprobe do_exit_kp = {
	.pre_handler = kprobes_handle_do_exit_pre,
	//.post_handler = kprobes_handle_do_exit_post,
	.symbol_name = "do_exit",
};
/*static struct kprobe do_fork_kp = {
	.pre_handler = kprobes_handle_do_fork_pre,
	.post_handler = kprobes_handle_do_fork_post,
	.symbol_name = "_do_fork",
};*/

void unregister_all(void)
{
	unregister_kprobe(&finish_task_switch_kp);
	unregister_kprobe(&do_exit_kp);
	//unregister_kprobe(&do_fork_kp);
}

int register_all(void)
{
	/* Register probes */
	
	int ret = register_kprobe(&finish_task_switch_kp);
	if (ret < 0)
	{
		printk(KERN_INFO "Couldn't register 'finish_task_switch' kprobe %d\n", ret);
		unregister_all();
		return (-EFAULT);
	}
	ret = register_kprobe(&do_exit_kp);
	if (ret < 0)
	{
		printk(KERN_INFO "Couldn't register 'do_exit' kprobe %d\n", ret);
		unregister_all();
		return (-EFAULT);
	}
	/*ret = register_kprobe(&do_fork_kp);
	if (ret < 0)
	{
		printk(KERN_INFO "Couldn't register 'do_fork' kprobe %d\n", ret);
		unregister_all();
		return (-EFAULT);
	}*/

	return (0);
}

/* Restart timer */
enum hrtimer_restart hrtimer_callback(struct hrtimer *timer)
{
	ktime_t kt_now;

	/* Restart timer */
	if (timer_restart && (counter < num_recordings))
	{
		/* Read counter */
		int i;
		if (sysmode)
		{
			for(i = 0; i < NUM_CORES; i++)
			{
				pmu_read_counters_core_oncpu(i);
			}
		}
		else
		{
			for(i = 0; i < target->index_size; i++)
			{
				if(target->target_pid[i].status && target->target_pid[i].on_cpu != -1 && target->target_pid[i].pid != 0)
				{
					pmu_read_counters_core_oncpu(target->target_pid[i].on_cpu);
				}
			}
		}
		pmu_read_counters();
		++counter;
		//printk(KERN_INFO "Extract value on CPU: %d, val: %d", current->on_cpu, hardware_events[4][counter-1]);	
	
		/* Forward timer */
		kt_now = hrtimer_cb_get_time(&hr_timer);
		hrtimer_forward(&hr_timer, kt_now, ktime_period_ns);

		return HRTIMER_RESTART;
	}
	/* No restart timer */
	else
	{
		if (!timer_restart)
		{
			printk("Timer Expired\n");
		}
		else if (counter > num_recordings)
		{
			printk("Counter > allowed spaces: %d > %d\n", counter, num_recordings);
		}
		return HRTIMER_NORESTART;
	}
}


/* Initialize module from ioctl start */
int start_counters()
{
	int i, j;
	
	if (!recording)
	{
		target->target_pid[0].pid = kleb_ioctl_args.pid;
		target->index_size += 1;
		printk(KERN_INFO "target pid: %d\n", (int)target->target_pid[0].pid);

		if(kleb_ioctl_args.pid == 1 || kleb_ioctl_args.pid == 0){
				sysmode = 1;
		}
		else{
				sysmode = 0;
		}

		/* Set empty buffer */
		for (i = 0; i < (num_events); ++i)
		{ 
			for (j = 0; j < num_recordings; ++j)
			{
				hardware_events[i][j] = -10;
			}
		}

		/* Initialize counters */
		pmu_start_counters();

		if(sysmode){
			for(i = 0; i < NUM_CORES; ++i){
				pmu_restart_counters(i);
			}	
		}

		ktime_period_ns = ktime_set(0, delay_in_ns);
		hrtimer_start(&hr_timer, ktime_period_ns, HRTIMER_MODE_REL);
		//printk(KERN_INFO "Timer start on PID: %d CPU: %d GCPU: %d", current->pid, current->on_cpu, get_cpu());
		recording = 1;
		timer_restart = 1;
		counter = 0;
	}
	else
	{
		printk(KERN_INFO "Invalid action: Counters already collecting\n");
	}

	return 0;
}

/* Deinitialize module from ioctl stop */
int stop_counters()
{
	int i;
	/* Stop counters */
	hrtimer_cancel(&hr_timer);

	for (i = 0; i < NUM_CORES; ++i)
	{
		pmu_stop_counters(i);
	}
	pmu_read_counters();
	++counter;
	recording = 0;
	timer_restart = 0;
	
	target->index_size = 0;
	
	return 0;
}

int open(struct inode *inode, struct file *fp)
{
	printk(KERN_INFO "Inside open\n");
	return 0;
}

/* Read for extract data to user */
ssize_t read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	int size_of_message = num_recordings * (num_events) * sizeof(unsigned int);
	int size_of_realmessage = counter * (num_events) * sizeof(unsigned int);
	int error_count = 0;
	int x, y;

	/* For periodic data extraction */
	if(recording){
		//FOR DEBUG
		//printk("Counter INT: %d On CPU: %d\n", counter, get_cpu());
		/* Read and save latest HPC */
		if(counter != 0){		

			/* Send data to user */
			error_count = copy_to_user(buffer, hardware_events[0], size_of_message);
			counter = 0;

			/* Reset buffer value */
			for (x = 0; x < (num_events); ++x)
			{ 
				for (y = 0; y < num_recordings; ++y)
				{
					hardware_events[x][y] = -10;
				}
			}
		}
	}
	/* For exit data extraction */
	else	
	{
		error_count = copy_to_user(buffer, hardware_events[0], size_of_message);
		if (cleanup_memory() < 0)
		{
			printk(KERN_INFO "Memory failed to cleanup cleanly");
		}
	}

	/* Check data extraction size */
	if (error_count == 0 && size_of_realmessage != 0)
	{
		//DEBUG
		return (size_of_realmessage);
	}
	else if(error_count == 0 && size_of_realmessage == 0){
		return (size_of_realmessage);
	}
	else
	{
		printk(KERN_INFO "Failed to send %d characters to the user\n", error_count);
		return -EFAULT;
	}
	
}

int release(struct inode *inode, struct file *fp)
{
	printk(KERN_INFO "Inside close\n");
	return 0;
}

#ifdef UNLOCKED
long ioctl_funcs(struct file *fp, unsigned int cmd, unsigned long arg)
#else
int ioctl_funcs(struct inode *inode, struct file *fp, unsigned int cmd, unsigned long arg)
#endif
{
	int ret = 0;
	kleb_ioctl_args_t *kleb_ioctl_args_user = (kleb_ioctl_args_t *)(arg);
	if (kleb_ioctl_args_user == NULL)
	{
		printk_d("lprof_ioctl: User did not pass in cmd\n");
		return (-EINVAL);
	}
	else{
		//DEBUG
		//printk(KERN_INFO "************ Catch IOCTL ***********\n");
		//printk(KERN_INFO "%u\n", cmd);
	}

	/* Read the parameters from userspace */
	if (copy_from_user(&kleb_ioctl_args, kleb_ioctl_args_user, sizeof(kleb_ioctl_args_t)) != 0)
	{
		printk_d("lprof_ioctl: Could not copy cmd from userspace\n");
		return (-EINVAL);
	}

	switch (cmd)
	{
		case IOCTL_DEFINE_COUNTERS:
			printk(KERN_INFO "This will define the counters\n");
			break;
		/* Start command */
		case IOCTL_START:
			printk(KERN_INFO "Starting counters\n");
			target_pid = kleb_ioctl_args.pid; 
			delay_in_ns = kleb_ioctl_args.delay_in_ns;
			num_events = kleb_ioctl_args.num_events;
			user_os_rec = kleb_ioctl_args.user_os_rec;

			if (initialize_memory() < 0)
			{
				printk(KERN_INFO "Memory failed to initialize");
				return (-ENODEV);
			}
			//DEBUG
			//printk(KERN_INFO "%d %d %d %d %llu %d\n",kleb_ioctl_args.counter1, kleb_ioctl_args.counter2, kleb_ioctl_args.counter3,kleb_ioctl_args.counter4, kleb_ioctl_args.counter_umask, kleb_ioctl_args.user_os_rec);
			start_counters();
			break;
		case IOCTL_DUMP:
			printk(KERN_INFO "This will dump the counters\n");
		break;
		/* Stop command */
		case IOCTL_STOP:
			printk(KERN_INFO "Stopping counters\n");
			stop_counters();
			break;
		case IOCTL_DELETE_COUNTERS:
			printk(KERN_INFO "This will delete the counters\n");
			break;
		case IOCTL_DEBUG:
			printk(KERN_INFO "This will set up debug mode\n");
			break;
		case IOCTL_STATS:
			printk(KERN_INFO "This will set up profiling mode\n");
			break;
		default:
			printk(KERN_INFO "Invalid command\n");
			break;
	}

	return ret;
}

#ifdef UNLOCKED
struct file_operations fops = {
	open : open,
	read : read,
	unlocked_ioctl : ioctl_funcs,
	release : release
};
#else
struct file_operations fops = {
	open : open,
	read : read,
	ioctl : ioctl_funcs,
	release : release
};
#endif

int initialize_memory()
{
	int i, j;

	printk("Memory initializing\n");

	//num_recordings = 500;
	/* Create Target id buffer */
	target = (target_array*)kmalloc(sizeof(target_array), GFP_KERNEL);
	target->size = 100;
	target->target_pid = (target_id*)kmalloc(sizeof(target_id)*target->size, GFP_KERNEL);

	/* Create data buffer */
	hardware_events = kmalloc((num_events) * sizeof(unsigned int *), GFP_KERNEL);
	hardware_events[0] = kmalloc((num_events) * num_recordings * sizeof(unsigned int), GFP_KERNEL);
	for (i = 0; i < (num_events); ++i)
	{ // This reduces the number of kmalloc calls
		hardware_events[i] = *hardware_events + num_recordings * i;
		for (j = 0; j < num_recordings; ++j)
		{
			hardware_events[i][j] = -10;
		}
	}
	return 0;
}

int initialize_timer()
{

	printk("Timer initializing\n");
	counter = 0;
	timer_restart = 0;
	printk("Number of Cores available %d\n", NUM_CORES);

	hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hr_timer.function = &hrtimer_callback;

	return 0;
}

int initialize_ioctl()
{
	int ret;
	dev_t dev_no, dev;

	printk("IOCTL initializing\n");

	kernel_cdev = cdev_alloc();
	kernel_cdev->ops = &fops;
	kernel_cdev->owner = THIS_MODULE;

	ret = alloc_chrdev_region(&dev_no, 0, 1, "char_arr_dev");
	if (ret < 0)
	{
		printk("Major number allocation has failed\n");
		return ret;
	}

	Major = MAJOR(dev_no);
	dev = MKDEV(Major, 0);
	printk("The major number for your device is %d\n", Major);

	ret = cdev_add(kernel_cdev, dev, 1);
	if (ret < 0)
	{
		printk(KERN_INFO "Unable to allocate cdev");
		return ret;
	}

	return 0;
}

int init_module(void)
{
	int ret;

	/* if (initialize_memory() < 0)
	{
		printk(KERN_INFO "Memory failed to initialize");
		return (-ENODEV);
	}*/

	if (initialize_timer() < 0)
	{
		printk(KERN_INFO "Timer failed to initialize");
		return (-ENODEV);
	}

	if (initialize_ioctl() < 0)
	{
		printk(KERN_INFO "IOCTL failed to initialize");
		return (-ENODEV);
	}

	ret = register_all();
	if (ret != 0)
	{
		return (ret);
	}

	printk("K-LEB module initialized\n");
	return 0;
}

int cleanup_memory()
{
	printk("Memory cleaning up\n");

    kfree(target->target_pid);
    kfree(target);
	kfree(hardware_events[0]);
	kfree(hardware_events);

	return 0;
}

int cleanup_timer()
{
	int ret;
	printk("Timer cleaning up\n");

	ret = hrtimer_cancel(&hr_timer);
	if (ret)
		printk("The timer was still in use...\n");

	return 0;
}
int cleanup_ioctl()
{
	printk("IOCTL cleaning up\n");

	cdev_del(kernel_cdev);
	unregister_chrdev_region(Major, 1);

	return 0;
}

void cleanup_module(void)
{
	/* if (cleanup_memory() < 0)
	{
		printk(KERN_INFO "Memory failed to cleanup cleanly");
	}*/

	if (cleanup_timer() < 0)
	{
		printk(KERN_INFO "Timer failed to cleanup cleanly");
	}

	if (cleanup_ioctl() < 0)
	{
		printk(KERN_INFO "IOCTL failed to cleanupcleanly");
	}

	unregister_all();

	printk("K-LEB module uninstalled\n");

	return;
}
