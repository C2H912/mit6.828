/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>



###################################################################
# exceptions/interrupts
###################################################################

/* TRAPHANDLER defines a globally-visible function for handling a trap.
 * It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 *
 * You shouldn't call a TRAPHANDLER function from C, but you may
 * need to _declare_ one in C (for instance, to get a function pointer
 * during IDT setup).  You can declare the function with
 *   void NAME();
 * where NAME is the argument passed to TRAPHANDLER.
 */
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text

/*
 * Lab 3: Your code here for generating entry points for the different traps.
 */
	// 我不敢保证TRAPHANDLER和TRAPHANDLER_NOEC都选对了，forgive me :(
	TRAPHANDLER_NOEC(Divide_Error, T_DIVIDE)
	TRAPHANDLER_NOEC(Debug_Exception, T_DEBUG)
	TRAPHANDLER_NOEC(Non_Maskable_Interrupt, T_NMI)
	TRAPHANDLER_NOEC(Breakpoint, T_BRKPT)
	TRAPHANDLER_NOEC(Overflow, T_OFLOW)
	TRAPHANDLER_NOEC(BOUND_Range_Exceeded, T_BOUND)
	TRAPHANDLER_NOEC(Invalid_Opcode, T_ILLOP)
	TRAPHANDLER_NOEC(Device_Not_Available, T_DEVICE)
	TRAPHANDLER_NOEC(Double_Fault, T_DBLFLT)
	TRAPHANDLER_NOEC(Invalid_Task_Switch_Segment, T_TSS)

	TRAPHANDLER(Segment_Not_Present, T_SEGNP)
	TRAPHANDLER(Stack_Exception, T_STACK)
	TRAPHANDLER(General_Protection_Fault, T_GPFLT)
	TRAPHANDLER(Page_Fault, T_PGFLT)

	TRAPHANDLER_NOEC(Floating_Point_Error, T_FPERR)

	TRAPHANDLER(Aligment_Check, T_ALIGN)

	TRAPHANDLER_NOEC(Machine_Check, T_MCHK)
	TRAPHANDLER_NOEC(SIMD_Floating_Point_Error, T_SIMDERR)

	TRAPHANDLER_NOEC(System_Call, T_SYSCALL)

/*
 * Lab 3: Your code here for _alltraps
 */
/*
 * 发生中断时，我们需要在栈上保存一个Trapframe结构体：
	|	ss		-------------------+
	|	esp					       |
	|	eflags				   CPU自动完成
	|	cs					       |
	|	eip					       |
	|	err (如果有的话) -----------+
	|	trapno				TRAPHANDLER和TRAPHANDLER_NOEC完成
	|	tf_padding2  --------------+
	|	ds						   |
	|	tf_padding1			 _alltraps需要完成的
	|	es						   |
	|	PushRegs  -----------------+
 */
_alltraps:
	// load GD_KD into %ds and %es
	// $GD_KD没办法直接movw给%ds和%es，所以借助%eax
	// 又因为%eax本身的值也要push到Trapframe中，所以不能破坏%eax的值
	pushl %eax
	movl $GD_KD, %eax
	movw %ax, %ds
	movw %ax, %es
	popl %eax

	pushw $0	// padding
	pushw %ds
	pushw $0	// padding
	pushw %es
	pushal

	// 设置函数参数并调用trap()
	pushl %esp
	call trap
