#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>

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

void
trap_init(void)
{
	extern struct Segdesc gdt[];

	// LAB 3: Your code here.
	// 初始化中断向量表，设置各种中断处理例程的对应入口
	SETGATE(idt[0], 1, GD_KT, &Divide_Error, 0);
	SETGATE(idt[1], 1, GD_KT, &Debug_Exception, 0);
	SETGATE(idt[2], 1, GD_KT, &Non_Maskable_Interrupt, 0);
	SETGATE(idt[3], 1, GD_KT, &Breakpoint, 3);
	SETGATE(idt[4], 1, GD_KT, &Overflow, 0);
	SETGATE(idt[5], 1, GD_KT, &BOUND_Range_Exceeded, 0);
	SETGATE(idt[6], 1, GD_KT, &Invalid_Opcode, 0);
	SETGATE(idt[7], 1, GD_KT, &Device_Not_Available, 0);
	SETGATE(idt[8], 1, GD_KT, &Double_Fault, 0);
	// idt[9] reserved (not generated by recent processors)
	SETGATE(idt[10], 1, GD_KT, &Invalid_Task_Switch_Segment, 0);
	SETGATE(idt[11], 1, GD_KT, &Segment_Not_Present, 0);
	SETGATE(idt[12], 1, GD_KT, &Stack_Exception, 0);
	SETGATE(idt[13], 1, GD_KT, &General_Protection_Fault, 0);
	SETGATE(idt[14], 1, GD_KT, &Page_Fault, 0);
	// idt[15] reserved
	SETGATE(idt[16], 1, GD_KT, &Floating_Point_Error, 0);
	SETGATE(idt[17], 1, GD_KT, &Aligment_Check, 0);
	SETGATE(idt[18], 1, GD_KT, &Machine_Check, 0);
	SETGATE(idt[19], 1, GD_KT, &SIMD_Floating_Point_Error, 0);

	// system call
	SETGATE(idt[48], 1, GD_KT, &System_Call, 3);

	// Per-CPU setup 
	trap_init_percpu();
}

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu(void)
{
	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	ts.ts_esp0 = KSTACKTOP;
	ts.ts_ss0 = GD_KD;
	ts.ts_iomb = sizeof(struct Taskstate);

	// Initialize the TSS slot of the gdt.
	gdt[GD_TSS0 >> 3] = SEG16(STS_T32A, (uint32_t) (&ts),
					sizeof(struct Taskstate) - 1, 0);
	gdt[GD_TSS0 >> 3].sd_s = 0;

	// Load the TSS selector (like other segment selectors, the
	// bottom three bits are special; we leave them 0)
	ltr(GD_TSS0);

	// Load the IDT
	lidt(&idt_pd);
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
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
		int32_t ret = syscall(tf->tf_regs.reg_eax, tf->tf_regs.reg_edx, tf->tf_regs.reg_ecx, 
							  tf->tf_regs.reg_ebx,tf->tf_regs.reg_edx, tf->tf_regs.reg_esi);
		if(ret < 0) {
			break;
		}
		tf->tf_regs.reg_eax = ret;	/* 读者可以思考一下为什么返回值放在这里 */
		return;		// 成功的话返回用户执行
	default:
		break;
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

	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	cprintf("Incoming TRAP frame at %p\n", tf);

	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		assert(curenv);

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

	// Return to the current environment, which should be running.
	assert(curenv && curenv->env_status == ENV_RUNNING);
	env_run(curenv);
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

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

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

