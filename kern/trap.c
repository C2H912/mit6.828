#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < ARRAY_SIZE(excnames))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

/*
 * 读者可能会对这里产生疑惑，因为这里只有中断函数的声明但没有定义，没有定义的
 * 中断函数怎么处理中断呢？实际上，这些函数都是有具体的实现的，只不过是用汇编
 * 去实现了 (写在trapentry.S中)，并没有用C语言实现，比如void Divide_Error()
 * 的对应实现为TRAPHANDLER_NOEC(Divide_Error, T_DIVIDE)。
 */
void Divide_Error();
void Debug_Exception();
void Non_Maskable_Interrupt();
void Breakpoint();
void Overflow();
void BOUND_Range_Exceeded();
void Invalid_Opcode();
void Device_Not_Available();
void Double_Fault();
void Invalid_Task_Switch_Segment();
void Segment_Not_Present();
void Stack_Exception();
void General_Protection_Fault();
void Page_Fault();
void Floating_Point_Error();
void Aligment_Check();
void Machine_Check();
void SIMD_Floating_Point_Error();
// system call
void System_Call();
// IRQ
void Clock_Interrupt();
void Irq_Kbd();
void Irq_Serial();
void Irq_Spurious();
void Irq_Ide();
void Irq_Error();

void
trap_init(void)
{
	extern struct Segdesc gdt[];

	// LAB 3: Your code here.
	// 初始化中断向量表，设置各种中断处理例程的对应入口
	SETGATE(idt[0], 0, GD_KT, &Divide_Error, 0);
	SETGATE(idt[1], 0, GD_KT, &Debug_Exception, 0);
	SETGATE(idt[2], 0, GD_KT, &Non_Maskable_Interrupt, 0);
	SETGATE(idt[3], 0, GD_KT, &Breakpoint, 3);
	SETGATE(idt[4], 0, GD_KT, &Overflow, 0);
	SETGATE(idt[5], 0, GD_KT, &BOUND_Range_Exceeded, 0);
	SETGATE(idt[6], 0, GD_KT, &Invalid_Opcode, 0);
	SETGATE(idt[7], 0, GD_KT, &Device_Not_Available, 0);
	SETGATE(idt[8], 0, GD_KT, &Double_Fault, 0);
	// idt[9] reserved (not generated by recent processors)
	SETGATE(idt[10], 0, GD_KT, &Invalid_Task_Switch_Segment, 0);
	SETGATE(idt[11], 0, GD_KT, &Segment_Not_Present, 0);
	SETGATE(idt[12], 0, GD_KT, &Stack_Exception, 0);
	SETGATE(idt[13], 0, GD_KT, &General_Protection_Fault, 0);
	SETGATE(idt[14], 0, GD_KT, &Page_Fault, 0);
	// idt[15] reserved
	SETGATE(idt[16], 0, GD_KT, &Floating_Point_Error, 0);
	SETGATE(idt[17], 0, GD_KT, &Aligment_Check, 0);
	SETGATE(idt[18], 0, GD_KT, &Machine_Check, 0);
	SETGATE(idt[19], 0, GD_KT, &SIMD_Floating_Point_Error, 0);

	// system call
	SETGATE(idt[48], 0, GD_KT, &System_Call, 3);

	// IRQ
	// 自从启动了IPQ后，我把所有的SETGATE的istrap参数都改成了0，why？见后文注释
	SETGATE(idt[IRQ_OFFSET + IRQ_TIMER], 0, GD_KT, &Clock_Interrupt, 3);
	SETGATE(idt[IRQ_OFFSET + IRQ_KBD], 0, GD_KT, &Irq_Kbd, 3);
	SETGATE(idt[IRQ_OFFSET + IRQ_SERIAL], 0, GD_KT, &Irq_Serial, 3);
	SETGATE(idt[IRQ_OFFSET + IRQ_SPURIOUS], 0, GD_KT, &Irq_Spurious, 3);
	SETGATE(idt[IRQ_OFFSET + IRQ_IDE], 0, GD_KT, &Irq_Ide, 3);
	SETGATE(idt[IRQ_OFFSET + IRQ_ERROR], 0, GD_KT, &Irq_Error, 3);

	// Per-CPU setup 
	trap_init_percpu();
}
/*
 * ---------- 所有的SETGATE的istrap参数都改成了0 ？
 *
 * 表面上看，这样改是不对的，因为这个参数的含义是：
 * 		"istrap: 1 for a trap (= exception) gate, 0 for an interrupt gate."
 * 这样改的话相当于把所有的exception都当成interrupt处理了。但是对于当前的设计来说，
 * 我们需要这样改，因为我们规定了外部设备中断在进入内核时始终是禁用的，仅在用户空间中
 * 启用。更具体来说，就是eflags寄存器的FL_IF标志位在进入内核时是不能为1的，我们可以
 * 在trap()函数中看到这一点：
 * 		assert(!(read_eflags() & FL_IF));
 * 
 * 然而在env.c的env_alloc()中，我们是把FL_IF位置为了1，说明在用户空间中启用了外部设备
 * 中断，那么在进入内核时，我们就需要把这个为置为0，并在返回用户态是恢复为1，怎么做到呢？
 * 很简单，SETGATE的istrap参数解释中给出：
 * 	   "see section 9.6.1.3 of the i386 reference: "The difference between
 * 		an interrupt gate and a trap gate is in the effect on IF (the
 * 		interrupt-enable flag). An interrupt that vectors through an
 * 		interrupt gate resets IF, thereby preventing other interrupts from
 * 		interfering with the current interrupt handler. A subsequent IRET
 * 		instruction restores IF to the value in the EFLAGS image on the
 * 		stack. An interrupt through a trap gate does not change IF."
 * 也就是说istrap=0时，发生中断时就会resets IF，退出中断时就会restores IF。
 * 
 * 所以我把istrap都改成了0。
 */

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// The example code here sets up the Task State Segment (TSS) and
	// the TSS descriptor for CPU 0. But it is incorrect if we are
	// running on other CPUs because each CPU has its own kernel stack.
	// Fix the code so that it works for all CPUs.
	//
	// Hints:
	//   - The macro "thiscpu" always refers to the current CPU's
	//     struct CpuInfo;
	//   - The ID of the current CPU is given by cpunum() or
	//     thiscpu->cpu_id;
	//   - Use "thiscpu->cpu_ts" as the TSS for the current CPU,
	//     rather than the global "ts" variable;
	//   - Use gdt[(GD_TSS0 >> 3) + i] for CPU i's TSS descriptor;
	//   - You mapped the per-CPU kernel stacks in mem_init_mp()
	//   - Initialize cpu_ts.ts_iomb to prevent unauthorized environments
	//     from doing IO (0 is not the correct value!)
	//
	// ltr sets a 'busy' flag in the TSS selector, so if you
	// accidentally load the same TSS on more than one CPU, you'll
	// get a triple fault.  If you set up an individual CPU's TSS
	// wrong, you may not get a fault until you try to return from
	// user space on that CPU.
	//
	// LAB 4: Your code here:
	uint8_t cpu_i = thiscpu->cpu_id;

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	thiscpu->cpu_ts.ts_esp0 = KSTACKTOP - cpu_i * (KSTKSIZE + KSTKGAP);
	thiscpu->cpu_ts.ts_ss0 = GD_KD;
	thiscpu->cpu_ts.ts_iomb = sizeof(struct Taskstate) + cpu_i * sizeof(struct Taskstate);

	// Initialize the TSS slot of the gdt.
	gdt[(GD_TSS0 >> 3) + cpu_i] = SEG16(STS_T32A, (uint32_t) (&thiscpu->cpu_ts), sizeof(struct Taskstate) - 1, 0);
	gdt[(GD_TSS0 >> 3) + cpu_i].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0 + (cpu_i << 3));

	// Load the IDT
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
			tf->tf_err & 4 ? "user" : "kernel",
			tf->tf_err & 2 ? "write" : "read",
			tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	if ((tf->tf_cs & 3) != 0) {
		cprintf("  esp  0x%08x\n", tf->tf_esp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions.
	// LAB 3: Your code here.
	int ret;
	switch (tf->tf_trapno) {
	case T_BRKPT:
		monitor(tf);	// 注意中断向量表中T_BRKPT的DPL要改为3
		return;
	case T_PGFLT:
		page_fault_handler(tf);
		return;
	case T_SYSCALL:
		// 跟系统调用有关系的就3个文件：inc/syscall.h, lib/syscall.c, kern/syscall.c
		// inc/syscall.h很简单，就定义了系统调用号
		// lib/syscall.c看文件夹名就知道，这是提供给用户调用的接口
		// kern/syscall.c看文件夹名就知道，这是内核处理用户请求的具体实现
		// 所以流程就是用户通过lib/syscall.c中的syscall()触发了中断(具体来说是int指令),内核识别到是系统
		// 调用的中断号，就走到这个case中，调用具体的处理函数(写在kern/syscall.c中)进行处理
		/*
		 * 这里很精妙，读者可以思考一下为什么syscall的参数是这些，答案在trap_dispatch()结尾注释给出
		 */
		ret = syscall(tf->tf_regs.reg_eax, tf->tf_regs.reg_edx, tf->tf_regs.reg_ecx, 
					  tf->tf_regs.reg_ebx, tf->tf_regs.reg_edi, tf->tf_regs.reg_esi);
		if(ret < 0) {
			break;
		}
		tf->tf_regs.reg_eax = ret;	/* 读者可以思考一下为什么返回值放在这里 */
		return;		// 成功的话返回用户执行
	default:
		break;
	}

	// 这是一个special case，因为我们在lib/ipc.c的ipc_send()中是要让sys_ipc_try_send()
	// 反复执行直到成功，如果不写这几句的话，一次失败就直接env_destroy()了。
	if(tf->tf_trapno == T_SYSCALL && tf->tf_regs.reg_eax == SYS_ipc_try_send) {
		if(ret == -7) {
			tf->tf_regs.reg_eax = ret;
			return;
		}
	}

	// Handle spurious interrupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		return;
	}

	// Handle clock interrupts. Don't forget to acknowledge the
	// interrupt using lapic_eoi() before calling the scheduler!
	// LAB 4: Your code here.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_TIMER) {
		lapic_eoi();
		sched_yield();
		return;
	}

	// Handle keyboard and serial interrupts.
	// LAB 5: Your code here.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_KBD) {
		kbd_intr();
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SERIAL) {
		serial_intr();
		return;
	}

	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}
/*
 * ------- 为什么syscall的参数是这些？
 *
 * 跟踪一下系统调用的流程可知，用户调用lib/syscall.c中的syscall()时会把函数参数放到eax等寄存器中。
 * 然后内核检测到中断发生，并根据中断向量号执行trapentry.S中对应的TRAPHANDLER_NOEC(System_Call, T_SYSCALL)
 * 代码，这段代码最后会执行到_alltraps中的pushal，也就是把eax等寄存器的值记录到Trapframe中的tf_regs结构体
 * 里面。所以syscall()的参数最后会在tf_regs中。
 * 
 * ------- 为什么syscall的返回值要这样写tf->tf_regs.reg_eax = ret？
 * 
 * 跟踪一下函数调用流程可知，把返回值存到tf->tf_regs.reg_eax后，trap_dispatch()返回trap()，
 * trap()调用env_run()，最后调用env_pop_tf()中的popal，也就是把tf->tf_regs的值赋值给eax等寄存器，
 * 所以返回值最后会放到eax寄存器里。然后用户态下的lib/syscall()继续执行，其中的汇编语言"=a" (ret)
 * 表明把eax寄存器中的值作为返回值，所以通过eax寄存器作为中介，用户能获取到内核syscall()的返回值。
 */

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Halt the CPU if some other CPU has called panic()
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// Re-acqurie the big kernel lock if we were halted in
	// sched_yield()
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();
	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		// Acquire the big kernel lock before doing any
		// serious kernel work.
		// LAB 4: Your code here.
		lock_kernel();
		assert(curenv);

		// Garbage collect if current enviroment is a zombie
		if (curenv->env_status == ENV_DYING) {
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}

		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}

	// Record that tf is the last real trapframe so
	// print_trapframe can print some additional information.
	last_tf = tf;

	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING)
		env_run(curenv);
	else
		sched_yield();
}

void mybp() {
	return;
}
void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();
	if(fault_va == 0) {
		mybp();
	}

	// Handle kernel-mode page faults.

	// LAB 3: Your code here.
	// Hint: to determine whether a fault happened in user mode or in kernel mode, 
	// check the low bits of the tf_cs.
	// 参考trap()中的if ((tf->tf_cs & 3) == 3)即可。
	// 具体解释来自env_alloc()中的注释:
	// 		"The low 2 bits of each segment register contains the
	//  	 Requestor Privilege Level (RPL); 3 means user mode."
	if((tf->tf_cs & 3) != 3) {
		print_trapframe(tf);
		panic("page_fault_handler: a page fault happens in kernel mode\n");
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// It is convenient for our code which returns from a page fault
	// (lib/pfentry.S) to have one word of scratch space at the top of the
	// trap-time stack; it allows us to more easily restore the eip/esp. In
	// the non-recursive case, we don't have to worry about this because
	// the top of the regular user stack is free.  In the recursive case,
	// this means we have to leave an extra word between the current top of
	// the exception stack and the new stack frame because the exception
	// stack _is_ the trap-time stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').

	// LAB 4: Your code here.

	/* 
	 * !!! 在你开始这个实验之前，请确保自己能完全理解Lab 3中的中断返回的机制(iret) !!!
	 *
	 * Lab 4 ex 9 ~ 10是个人感觉高难且易错的部分，我尽量给大家讲清楚这部分，
	 * 各位可以自己先尝试一下，suffer from debugginnnnnng XD，讲解将在本函数尾给出。
	 */

	if(curenv->env_pgfault_upcall != NULL) {
		//mybp();

		uint32_t *uxstack_ptr = NULL;
		// (1) tf_esp < USTACKTOP说明是从用户栈首次切换到异常栈，所以要将esp更新为异常栈的开始
		if(curenv->env_tf.tf_esp < USTACKTOP) {
			uxstack_ptr = (uint32_t*)(UXSTACKTOP - 4);	// 注意这里是减去4个字节
		}
		// (2) 当前已经在异常栈上了，又发生了page fault，也就是异常的嵌套，直接在异常栈
		//     上开启一个新的栈帧，curenv->env_tf.tf_esp即指向了异常栈的栈顶
		else {
			uxstack_ptr = (uint32_t*)curenv->env_tf.tf_esp;
			uxstack_ptr -= 2;							// 注意要预留一个32-bit word用于存放返回地址
		}

		// 检查当前我们要写入UTrapframe的内存的合法性
		user_mem_assert(curenv, (void*)(uxstack_ptr - 12), 13 * 4, PTE_P | PTE_U | PTE_W);

		// 压入struct UTrapframe
		*uxstack_ptr = curenv->env_tf.tf_esp;
		*(uxstack_ptr - 1) = curenv->env_tf.tf_eflags;
		*(uxstack_ptr - 2) = curenv->env_tf.tf_eip;

		*(uxstack_ptr - 3) = curenv->env_tf.tf_regs.reg_eax;
		*(uxstack_ptr - 4) = curenv->env_tf.tf_regs.reg_ecx;
		*(uxstack_ptr - 5) = curenv->env_tf.tf_regs.reg_edx;
		*(uxstack_ptr - 6) = curenv->env_tf.tf_regs.reg_ebx;
		*(uxstack_ptr - 7) = curenv->env_tf.tf_regs.reg_oesp;
		*(uxstack_ptr - 8) = curenv->env_tf.tf_regs.reg_ebp;
		*(uxstack_ptr - 9) = curenv->env_tf.tf_regs.reg_esi;
		*(uxstack_ptr - 10) = curenv->env_tf.tf_regs.reg_edi;

		*(uxstack_ptr - 11) = curenv->env_tf.tf_err;
		*(uxstack_ptr - 12) = fault_va;
		
		// 参考官方文档："fault_va   <-- %esp when handler is run"
		curenv->env_tf.tf_esp = (uintptr_t)(uxstack_ptr - 12);
		// 检查用户程序入口合法性，设置中断返回后的用户程序入口
		user_mem_assert(curenv, (void*)(curenv->env_pgfault_upcall), PGSIZE, PTE_P | PTE_U);
		curenv->env_tf.tf_eip = (uintptr_t)curenv->env_pgfault_upcall;
		
		env_run(curenv);
	}

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}
/*
 * --------------- Lab 4 ex 9讲解 
 *
 * 首先，我们要知道这里到底要我们实现什么东西，也就是
 * 		User-level page fault handling
 * 以往每次发生页错误时，都触发中断，然后交给内核的中断处理程序去处理错误。
 * 然而，我们可以把如何去处理的权力交给用户，让用户自己决定到底该怎么处理页错误，
 * 这样会有非常大的灵活性。
 * 
 * 那么怎么去实现这个功能呢？我们可以先回顾一下传统上的内核处理的流程：
 * 
 * 		1. 用户执行到错误代码，触发中断
 *  	2. 内核响应中断，用户态切换到内核态，用户栈切换到内核栈
 *  	3. 各种信息 (struct Trapframe) 保存到内核栈中
 *  	4. 运行内核中写好的中断处理程序
 *  	5. 中断返回，内核态切换回用户态，内核栈切换回用户栈，恢复用户程序出错时状态
 * 	 	6. 用户程序继续运行下一条指令
 * 
 * 一眼看过去好像只需要把第4步改成调用用户自定义的处理程序就好了，多简单~
 * 但是！现在是处于内核态中的，也就是说这时候运行的程序具有极高的权限，而我们运行
 * 的又是用户程序，如果用户程序故意写一段清空磁盘的代码，那么...
 * 
 * 所以，正确的流程大概是这个样子的 (暂时) :
 * 
 * 		1. 用户执行到错误代码，触发中断
 *  	2. 内核响应中断，用户态切换到内核态，用户栈切换到内核栈
 *  	3. 各种信息 (struct Trapframe) 保存到内核栈中
 *  	4. 中断返回，内核态切换回用户态，内核栈切换回用户栈，恢复用户程序出错时状态
 * 		5. < 运行用户自定义的处理程序 >
 * 	 	6. 用户程序继续运行下一条指令
 * 
 * 这样前面5步看起来都没有任何问题，但是不能顺利执行第6步了 :( ，为什么？因为
 * 我们需要先还原各种寄存器的状态，才能继续执行下一条指令。而我们在第3步中保存的状态
 * 在第4步时已经把它们还原到寄存器上了，然后第5步运行处理程序时，肯定会把寄存器的状态
 * 破坏掉，这样第6步就没法运行了。
 * 
 * 所以，我们需要把第3步中保存下来的信息，转存到某个地方，这样在第5步执行完成后，用
 * 它们来恢复寄存器的状态。正确的流程大概是这个样子的：
 * 
 * 		1. 用户执行到错误代码，触发中断
 *  	2. 内核响应中断，用户态切换到内核态，用户栈切换到内核栈
 *  	3. 各种信息 (struct Trapframe) 保存到内核栈中，< 并转存到某个用户可以访问到的地方 >
 *  	4. 中断返回，内核态切换回用户态，内核栈切换回用户栈
 * 		5. < 运行用户自定义的处理程序，运行完成后恢复出错时的状态 > 
 * 	 	6. 用户程序继续运行下一条指令
 * 
 * 具体来说，"某个用户可以访问到的地方"就是用户异常栈(见memlayout.h中的User Exception Stack)，
 * 而保存下来的信息我们不用struct Trapframe，而是用struct UTrapframe。这下你回看上面的代码，
 * 其实就很清晰了，无非就是将Trapframe的东西拷贝到User Exception Stack中，使这个Stack看起来是
 * 一个UTrapframe结构体，然后更改Trapframe中的tf_esp和tf_eip，通过iret机制跳到用户处理程序和
 * Exception Stack中执行而已。有个边界条件就是要区分一下是首次page fault还是page fault嵌套。
 * 至于如何恢复出错时的状态，继续运行下一条指令，就是ex 10的内容了。
 * 
 * 最后，你会发现一些"奇怪"的代码，比如：
 * 		*uxstack_ptr = curenv->env_tf.tf_esp - 4;
 * 为什么不是*uxstack_ptr = curenv->env_tf.tf_esp而是还要减4呢？这就是ex 10中精妙的地方了!
 * 
 * 
 * --------------- Lab 4 ex 10讲解 
 * 
 * 书接上回，我们在第5步时会跳到用户自定义的处理程序中执行，也就是pfentry.S中的_pgfault_upcall，
 * 当然，实际的处理程序是在_pgfault_handler中，_pgfault_upcall一开始无非就是call了一下_pgfault_handler，
 * 处理程序结束后，自然就是恢复出错时的状态，继续运行下一条指令，这就是ex 10要求我们完成的东西。
 * 
 * 要恢复哪些东西？显然是struct PushRegs，eflags和esp。怎么恢复？pop出来或mov就可以了。
 * 
 * 怎么继续运行下一条指令？对于中断返回来说，设置tf_eip然后iret就可以了；但是对于用户处理程序来说，因为现在
 * 是在用户态，自然就不能借助iret指令了。那么我们可以想到跳转指令，比如jmp，然而...
 * 
 *  	"We can't call 'jmp', since that requires that we load the address into a register, 
 * 		 and all registers must have their trap-time values after the return."
 * 
 * 意思就是如果使用jmp，那么会改变我们刚才已经恢复好的寄存器的状态，也就是跳转虽然成功了，但是寄存器的值
 * 又无效了...
 * 
 * 所以现在难就难在，怎么在不破坏已经恢复好的寄存器值的前提下，实现跳转？官方也是这么说的：
 * 		
 * 		"The interesting part is returning to the original point in the user code that caused 
 * 		 the page fault. You'll return directly there, without going back through the kernel. 
 * 		 The hard part is simultaneously switching stacks and re-loading the EIP."
 * 
 * "switching stacks"意思就是指把esp寄存器的值设置为正确的栈顶。
 * 
 * 提示：使用ret指令。ret指令主要做了两件事：
 * 		1. 将当前esp指向的栈顶的内容赋值给eip寄存器
 * 		2. esp++
 * 
 * 下面我将用"动画"的形式讲解下关键思想，请不要在意各种实现的细节：
 * 
 * <时刻1. 用户程序发生page fault>			<时刻2. 用户处理程序开始运行>	
 * 
 * 				内存
 * 			+--------------+					+--------------+
 * 			|              |					|              |			   
 * 			|用户正常使用   | 					 |用户正常使用   |
 *			|的栈          |				    | 的栈          | 
 * 			|--------------|<----原esp			|--------------|
 * 			|			   |					|留空的4字节    |
 * 			|			   |					|--------------|
 * 			|              |					|异常栈         |
 * 			|              |					|UTrapFrame    |
 * 			| NULL /////// |					|			   |
 * 			|			   |					|--------------|<----现esp
 * 			|              |					| NULL /////// |
 * 			|--------------|					|--------------|
 * 			|用户处理程序   |					 |用户处理程序   |<----现eip
 * 			|              |					|			   |
 * 			|--------------|					|--------------|
 * 			|发生错误时的   |<----原eip			 |发生错误时的   |
 * 			|代码          |					| 代码          |
 * 			|              |					|			   |
 *          +—-------------+					+--------------+
 * 
 * <时刻3. 用户处理程序完成，pfrentry.S中刚执行完addl $4, %esp>	
 * 
 * 				内存
 * 			+--------------+					
 * 			|              |							   
 * 			|用户正常使用   | 					 
 *			|的栈          |				     
 * 			|--------------|		
 * 			|留空的4字节	|				
 * 			|--------------|					
 * 			|异常栈         |					
 * 			|UTrapFrame    |			
 * 			|--------------|<----现esp						
 * 			|NULL ///////  |				
 * 			|--------------|					
 * 			|用户处理程序   |		eip现在指向	addl $4, %esp之后的第一条汇编	
 * 			|              |					
 * 			|--------------|					
 * 			|发生错误时的   |		
 * 			|代码          |					
 * 			|              |					
 *          +—-------------+
 * 
 * <时刻4. pfrentry.S中执行到addl $8, %esp>	
 * 
 * 				内存
 * 			+--------------+					
 * 			|              |							   
 * 			|用户正常使用   | 					 
 *			|的栈           |				     
 * 			|--------------|		
 * 			|原eip值    	|				
 * 			|--------------|					
 * 			|异常栈         |					
 * 			|UTrapFrame    |			
 * 			|--------------|<----现esp						
 * 			|NULL ///////  |				
 * 			|--------------|					
 * 			|用户处理程序   |		eip现在指向	addl $8, %esp	
 * 			|              |					
 * 			|--------------|					
 * 			|发生错误时的   |		
 * 			|代码           |					
 * 			|              |					
 *          +—-------------+
 * 
 * <时刻5. pfrentry.S中执行到ret>
 * 注意！这时候已经把UTrapFrame中PushRegs和eflags的值还原好了，
 * 但esp并没有！对比时刻1的图你会发现，现esp与原esp差了4字节。
 * 
 * 				内存
 * 			+--------------+					
 * 			|              |							   
 * 			|用户正常使用   | 					 
 *			|的栈           |				     
 * 			|--------------|		
 * 			|原eip值    	|				
 * 			|--------------|<----现esp					
 * 			|              |					
 * 			|NULL ///////  |			
 * 			|              |						
 * 			|			   |				
 * 			|--------------|					
 * 			|用户处理程序   |		eip现在指向	ret	
 * 			|              |					
 * 			|--------------|					
 * 			|发生错误时的   |		
 * 			|代码           |					
 * 			|              |					
 *          +—-------------+
 * 
 * <时刻6. 执行ret，魔法发生！>
 * ret指令第一步先将当前esp指向的栈顶的内容赋值给eip寄存器，观察时刻5的图，你会发现这正好
 * 把原eip的值赋值给了eip寄存器，也就是eip寄存器现在指向了时刻1图中"原eip"的地方！
 * 
 * ret指令第二步把现在的esp++，正好往上移动4字节，这不正好回到了时刻1图中"原esp"的地方？！
 * 
 * 这样，通过一条ret指令，就实现了"simultaneously switching stacks and re-loading the EIP"
 * 
 * 这下，时刻6之后是不是变回了时刻1时候的状态了？并且所有寄存器的值都被还原了，这不就实现了
 * 		"恢复出错时的状态，继续运行下一条指令"
 * 的要求了吗？
 * 
 * 操作系统实在太神奇了(笑		
 */