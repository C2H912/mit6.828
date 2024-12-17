#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/spinlock.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>

void sched_halt(void);

// Choose a user environment to run and run it.
//
// Implement simple round-robin scheduling.
//
// Search through 'envs' for an ENV_RUNNABLE environment in
// circular fashion starting just after the env this CPU was
// last running.  Switch to the first such environment found.
//
// If no envs are runnable, but the environment previously
// running on this CPU is still ENV_RUNNING, it's okay to
// choose that environment.
//
// Never choose an environment that's currently running on
// another CPU (env_status == ENV_RUNNING). If there are
// no runnable environments, simply drop through to the code
// below to halt the cpu.
void
sched_yield(void)
{
	struct Env *idle;

	// LAB 4: Your code here.

	// (1) 先运行ENV_RUNNABLE的程序

	// 注意首次加载的边界条件!!!
	if(thiscpu->cpu_env == NULL) {
		for(size_t i = 0; i < NENV; i++) {
			if(envs[i].env_status == ENV_RUNNABLE) {
				thiscpu->cpu_env = &envs[i];
				env_run(thiscpu->cpu_env);	// 注意env_run()永不返回
			}
		}
	}
	// 非首次加载，从"starting just after..."的位置开始找
	else {
		size_t start = (thiscpu->cpu_env->env_id + 1) % NENV;
		size_t search_len = 0;
		while(search_len <= NENV - 1) {
			size_t cur_id = (start + search_len) % NENV;
			if(envs[cur_id].env_status == ENV_RUNNABLE) {
				thiscpu->cpu_env = &envs[cur_id];
				env_run(thiscpu->cpu_env);
			}
			search_len++;
		}
	}

	// If no envs are runnable, but the environment previously running on this 
	// CPU is still ENV_RUNNING, it's okay to choose that environment.
	// (2) 如果没有ENV_RUNNABLE的程序了，再运行ENV_RUNNING并且是该CPU之前运行过的程序

	// 请读者思考一下case(2)的用意是什么，什么情况下会进入到这里，解释放在函数尾的注释中。
	static size_t start = 0;
	size_t search_len = 0;
	while(search_len <= NENV - 1) {
		size_t cur_id = (start + search_len) % NENV;
		if(envs[cur_id].env_status == ENV_RUNNING && envs[cur_id].env_cpunum == thiscpu->cpu_id) {
			thiscpu->cpu_env = &envs[cur_id];
			start = (cur_id + 1) % NENV;
			env_run(thiscpu->cpu_env);
		}
		search_len++;
	}

	// sched_halt never returns
	// (3) 没有可以运行的程序了，就让CPU halt
	sched_halt();
}
/*
 * --------- case 2的用意
 *
 * 假设现在有一个核，并创建了两个用户环境(都处于ENV_RUNNABLE)，这两个用户环境都会调用sys_yield()。
 * 	   时刻0: CPU 0调用env 0，env 0的状态变为ENV_RUNNING
 *     时刻1: env 0调用sys_yield()，CPU 0找到下一个RUNNABLE的环境，即env 1，并其状态改为ENV_RUNNING
 *     时刻2: env 1调用sys_yield()，CPU 0无法找到ENV_RUNNABLE的环境了，如果没有case 2，只能halt，
 * 	         但这个时候env 0和env 1实际上都没有运行结束
 * 
 * 所以case 2的语意为"哪个CPU将env从RUNNABLE转换到RUNNING的，就要负责到底，把env运行完成"，
 * 简单来说就是"谁启动谁完成"，一个任务只能交由一个CPU完成。
 */

// Halt this CPU when there is nothing to do. Wait until the
// timer interrupt wakes it up. This function never returns.
//
void
sched_halt(void)
{
	int i;

	// For debugging and testing purposes, if there are no runnable
	// environments in the system, then drop into the kernel monitor.
	for (i = 0; i < NENV; i++) {
		if ((envs[i].env_status == ENV_RUNNABLE ||
		     envs[i].env_status == ENV_RUNNING ||
		     envs[i].env_status == ENV_DYING))
			break;
	}
	if (i == NENV) {
		cprintf("No runnable environments in the system!\n");
		while (1)
			monitor(NULL);
	}

	// Mark that no environment is running on this CPU
	curenv = NULL;
	lcr3(PADDR(kern_pgdir));

	// Mark that this CPU is in the HALT state, so that when
	// timer interupts come in, we know we should re-acquire the
	// big kernel lock
	xchg(&thiscpu->cpu_status, CPU_HALTED);

	// Release the big kernel lock as if we were "leaving" the kernel
	unlock_kernel();

	// Reset stack pointer, enable interrupts and then halt.
	asm volatile (
		"movl $0, %%ebp\n"
		"movl %0, %%esp\n"
		"pushl $0\n"
		"pushl $0\n"
		// Uncomment the following line after completing exercise 13
		//"sti\n"
		"1:\n"
		"hlt\n"
		"jmp 1b\n"
	: : "a" (thiscpu->cpu_ts.ts_esp0));
}

