/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

struct Env *envs = NULL;		// All environments
static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)

#define ENVGENSHIFT	12		// >= LOGNENV

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
struct Segdesc gdt[NCPU + 5] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// Per-CPU TSS descriptors (starting from GD_TSS0) are initialized
	// in trap_init_percpu()
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the env_free_list.
// Make sure the environments are in the free list in the same order
// they are in the envs array (i.e., so that the first call to
// env_alloc() returns envs[0]).
//
void
env_init(void)
{
	// Set up envs array
	// LAB 3: Your code here. Exercise 2
	env_free_list = NULL;
	// 从末尾反向遍历到开头即可实现上述“Make sure the environments...”的要求
	for(int i = NENV - 1; i >= 0; i--) {
		envs[i].env_status = ENV_FREE; 
		envs[i].env_id = 0;
		envs[i].env_parent_id = 0;
		envs[i].env_pgdir = NULL;
		envs[i].env_runs = 0;
		envs[i].env_link = env_free_list;
		env_free_list = &envs[i];
		memset(&envs[i].env_tf, 0, sizeof(envs[i].env_tf));
	}
	/*
	 * 上面的for循环，我之前的写法是for(size_t i = NENV - 1; i >= 0; i--)，
	 * 读者能看出有什么bug吗？没错，当i=0时，i--会发生下溢，i的值变为4294967295，
	 * 满足i>=0，然后数组访问越界 :(
	 */

	// Per-CPU part of the initialization
	env_init_percpu();
}

// Load GDT and segment descriptors.
void
env_init_percpu(void)
{
	lgdt(&gdt_pd);
	// The kernel never uses GS or FS, so we leave those set to
	// the user data segment.
	asm volatile("movw %%ax,%%gs" : : "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" : : "a" (GD_UD|3));
	// The kernel does use ES, DS, and SS.  We'll change between
	// the kernel and user data segments as needed.
	asm volatile("movw %%ax,%%es" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));
	// Load the kernel text segment into CS.
	asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT));
	// For good measure, clear the local descriptor table (LDT),
	// since we don't use it.
	lldt(0);
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page directory, set e->env_pgdir accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
static int
env_setup_vm(struct Env *e)
{
	int i;
	struct PageInfo *p = NULL;

	// Allocate a page for the page directory
	if (!(p = page_alloc(ALLOC_ZERO)))
		return -E_NO_MEM;

	// Now, set e->env_pgdir and initialize the page directory.
	//
	// Hint:
	//    - The VA space of all envs is identical above UTOP
	//	(except at UVPT, which we've set below).
	//	See inc/memlayout.h for permissions and layout.
	//	Can you use kern_pgdir as a template?  Hint: Yes.
	//	(Make sure you got the permissions right in Lab 2.)
	//    - The initial VA below UTOP is empty.
	//    - You do not need to make any more calls to page_alloc.
	//    - Note: In general, pp_ref is not maintained for
	//		physical pages mapped only above UTOP, but env_pgdir
	//		is an exception -- you need to increment env_pgdir's
	//		pp_ref for env_free to work correctly.
	//    - The functions in kern/pmap.h are handy.

	// LAB 3: Your code here.
	// you need to increment env_pgdir's pp_ref for env_free to work correctly.
	p->pp_ref++;
	// set e->env_pgdir
	e->env_pgdir = page2kva(p);	
	// initialize the kernel portion of the new environment's address space
	/*
	 * 这里有两个值得思考的问题：
	 * 	  Q1：为什么要把内核的空间也映射用户的页目录表中？
	 * 	  Q2：为什么这里不用boot_map_region()?
	 * 答案将在本函数后的注释中给出。
	 */
	e->env_pgdir[PDX(UENVS)] = kern_pgdir[PDX(UENVS)];
	e->env_pgdir[PDX(UPAGES)] = kern_pgdir[PDX(UPAGES)];
	e->env_pgdir[PDX(KSTACKTOP-KSTKSIZE)] = kern_pgdir[PDX(KSTACKTOP-KSTKSIZE)];
	// 这里要牢记要映射的范围的大小到底是多少！曾经的我只写了一行：
	// 		e->env_pgdir[PDX(KERNBASE)] = kern_pgdir[PDX(KERNBASE)];
	// 然后访问0xf7e6aff4时就发生内核态的Page Fault了，debug了一整天 :)
	size_t size = 0x100000000 - KERNBASE;
	size_t total_dir_count = size / PTSIZE;	// 注意不是PGSIZE，因为是页目录表不是页表
	size_t count = 0;
	while(count < total_dir_count) {
		e->env_pgdir[PDX(KERNBASE + PTSIZE * count)] = kern_pgdir[PDX(KERNBASE + PTSIZE * count)];
		count++;
	}
	// 在Lab 4之前，不写这一句都是OK的，因为没有用到Memory-mapped I/O中的东西。
	// 但是Lab 4后，MMIO中的东西就被用上了，比如cpunum()的代码要访问lapic，而lapic是在MMIO中的，
	// 所以要加入这条映射。这个bug又花了我一天的时间 :(
	e->env_pgdir[PDX(MMIOBASE)] = kern_pgdir[PDX(MMIOBASE)];

	// UVPT maps the env's own page table read-only.
	// Permissions: kernel R, user R
	e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;

	return 0;
}
/*
 * ------- Q1：为什么要把内核的空间也映射用户的页目录表中？
 * 
 * 一句话回答就是<在某些情况下，用户需要访问内核空间中的代码或数据>。你可能会说：“让用户访问内核
 * 不会造成安全问题吗？”当然，如果让用户“随意地”访问，肯定是不可以的。然而在某些情况下，比如说异常，
 * 或者系统调用，用户自己是没办法处理，他就需要调用内核中的中断处理程序来协助完成工作，所以这时候，
 * 用户是需要借助页目录表和页表帮忙访问到内核的空间的。同时，为了安全起见，这部分是只有在内核态下才
 * 可以访问到的（页目录表和页表中的PTE位会检查权限!），平时在用户态下是没办法执行的，特权级的切换
 * 保证了安全性。操作系统的设计就是这么的精妙！
 * 
 * ------- Q2：为什么这里不用boot_map_region()?
 * 
 * 首先boot_map_region()是被static所修饰的，也就是说在这个文件中是“看不到”这个函数的，
 * 自然从语法上就不能去调用这个函数。
 * 
 * 但是，假设我们把这个static去掉，让boot_map_region()能被调用，我们就应该用这个函数了吗？
 * 
 * 答案还是否定的。因为这里只是把内核的部分映射到用户的地址空间中，对于每个用户来说，内核部分
 * 是无权修改的，且每个用户看到的这部分内容都应该是一样的，所以直接让用户去访问kern_pgdir的
 * 对应页表即可。如果使用boot_map_region()的话，会给每个用户创建独属于自己的页表去维护这些
 * 相同的的映射，这样会产生没必要的内存开销 (因为每个用户看到的这部分内容都是一样的！明明只需
 * 要一份就行却创建了多个相同的副本)，而且如果这部分映射发生了变动，就需要同时修改所有的副本，
 * 造成额外的性能开销。
 */

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENV environments are allocated
//	-E_NO_MEM on memory exhaustion
//
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// Generate an env_id for this environment.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and
	// GD_UT is the user text segment selector (see inc/memlayout.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.  When
	// we switch privilege levels, the hardware does various
	// checks involving the RPL and the Descriptor Privilege Level
	// (DPL) stored in the descriptors themselves.
	e->env_tf.tf_ds = GD_UD | 3;
	e->env_tf.tf_es = GD_UD | 3;
	e->env_tf.tf_ss = GD_UD | 3;
	e->env_tf.tf_esp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | 3;
	// You will set e->env_tf.tf_eip later.

	// Enable interrupts while in user mode.
	// LAB 4: Your code here.
	e->env_tf.tf_eflags |= FL_IF;

	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	// commit the allocation
	env_free_list = e->env_link;
	*newenv_store = e;

	cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Does not zero or otherwise initialize the mapped pages in any way.
// Pages should be writable by user and kernel.
// Panic if any allocation attempt fails.
//
static void
region_alloc(struct Env *e, void *va, size_t len)
{
	// LAB 3: Your code here.
	// (But only if you need it for load_icode.)
	//
	// Hint: It is easier to use region_alloc if the caller can pass
	//   'va' and 'len' values that are not page-aligned.
	//   You should round va down, and round (va + len) up.
	//   (Watch out for corner-cases!)
	uint32_t cur_va = (uint32_t) ROUNDDOWN(va, PGSIZE);
	while(cur_va < (uint32_t)((size_t)va + len)) {
		struct PageInfo *page = page_alloc(0);
		if(page == NULL) {
			panic("region_alloc: page_alloc() fail\n");
		}
		int ret = page_insert(e->env_pgdir, page, (void*)cur_va, PTE_U | PTE_W);
		if(ret != 0) {
			panic("region_alloc: user page_insert() fail\n");
		}
		// Pages should be writable by user and kernel.
		// 为什么明明是分配用户空间，内核也要管理这样一份副本呢？
		// 因为我们现在还运行在内核态下，在OS从内核正式切换到用户态去执行的过程中，
		// 内核需要对这部分用户空间进行一些操作，比如load_icode()中的memcpy，
		// 如果不同时映射到kern_pgdir，内核就没有办法去操作这部分内存空间
		ret = page_insert(kern_pgdir, page, (void*)cur_va, PTE_U | PTE_W);
		if(ret != 0) {
			panic("region_alloc: kern page_insert() fail\n");
		}
		cur_va += PGSIZE;
	}
}

// 这是我之前的一个错误的写法，读者可以看看问题出在哪里，答案在函数尾给出
static void
WRONG_region_alloc_DONNOT_USE(struct Env *e, void *va, size_t len)
{
	uint32_t cur_va = (uint32_t) ROUNDDOWN(va, PGSIZE);
	for(size_t i = 0; i < len; i += PGSIZE, cur_va += PGSIZE) {
		struct PageInfo *page = page_alloc(0);
		if(page == NULL) {
			panic("region_alloc: page_alloc() fail\n");
		}
		int ret = page_insert(e->env_pgdir, page, (void*)cur_va, PTE_U | PTE_W);
		if(ret != 0) {
			panic("region_alloc: page_insert() fail\n");
		}
		ret = page_insert(kern_pgdir, page, (void*)cur_va, PTE_U | PTE_W);
		if(ret != 0) {
			panic("region_alloc: kern page_insert() fail\n");
		}
	}
}
// BUG：因为目的是要映射[va, va+len)的地址空间，这样写实际上只映射了
// [ROUNDDOWN(va), ROUNDDOWN(va)+len)的空间，而ROUNDDOWN(va)+len在
// 很多情况下都小于va+len，那么程序在访问ROUNDDOWN(va)+len到va+len的
// 空间时，这部分是没有映射的，所以会发生错误。

//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// All this is very similar to what our boot loader does, except the boot
// loader also needs to read the code from disk.  Take a look at
// boot/main.c to get ideas.
//
// Finally, this function maps one page for the program's initial stack.
//
// load_icode panics if it encounters problems.
//  - How might load_icode fail?  What might be wrong with the given input?
//
static void
load_icode(struct Env *e, uint8_t *binary)
{
	// Hints:
	//  Load each program segment into virtual memory
	//  at the address specified in the ELF segment header.

	//
	//  All page protection bits should be user read/write for now.
	//  ELF segments are not necessarily page-aligned, but you can
	//  assume for this function that no two segments will touch
	//  the same virtual page.
	//
	//  You may find a function like region_alloc useful.
	//
	//  Loading the segments is much simpler if you can move data
	//  directly into the virtual addresses stored in the ELF binary.
	//  So which page directory should be in force during
	//  this function?
	//
	//  You must also do something with the program's entry point,
	//  to make sure that the environment starts executing there.
	//  What?  (See env_run() and env_pop_tf() below.)

	// LAB 3: Your code here.
	struct Elf * elf_hdr = (struct Elf*) binary;
	// is this a valid ELF?
	if (elf_hdr->e_magic != ELF_MAGIC) {
		panic("load_icode: elf_hdr->e_magic != ELF_MAGIC\n");
	}

	struct Proghdr *ph = (struct Proghdr *) ((uint8_t *) elf_hdr + elf_hdr->e_phoff);
	struct Proghdr *eph = ph + elf_hdr->e_phnum;
	for (; ph < eph; ph++) {
		//  You should only load segments with ph->p_type == ELF_PROG_LOAD.
		if(ph->p_type == ELF_PROG_LOAD) {
			//  Each segment's virtual address can be found in ph->p_va
			//  and its size in memory can be found in ph->p_memsz.
			region_alloc(e, (void*)ph->p_va, ph->p_memsz);
			//  The ph->p_filesz bytes from the ELF binary, starting at
			//  'binary + ph->p_offset', should be copied to virtual address ph->p_va. 
			memcpy((void*)ph->p_va, binary + ph->p_offset, ph->p_filesz);
			//  Any remaining memory bytes should be cleared to zero.
			//  (The ELF header should have ph->p_filesz <= ph->p_memsz.)
			if(ph->p_filesz > ph->p_memsz) {
				panic("load_icode: ph->p_filesz > ph->p_memsz\n");
			}
			memset((void*)ph->p_va + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
		}
	}

	// 设置函数入口。为什么把入口设置在这里？看完env_run()和env_pop_tf()后你就知道了
	// 详细的讲解将在env_run()后面的注释中给出
	e->env_tf.tf_eip = (uintptr_t)elf_hdr->e_entry;

	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.
	// LAB 3: Your code here.
	region_alloc(e, (void*)(USTACKTOP - PGSIZE), PGSIZE);
}

//
// Allocates a new env with env_alloc, loads the named elf
// binary into it with load_icode, and sets its env_type.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
//
void
env_create(uint8_t *binary, enum EnvType type)
{
	// LAB 3: Your code here.
	struct Env *new_env;
	if(env_alloc(&new_env, 0) != 0) {
		panic("env_create: env_alloc(&new_env, 0) fail\n");
	}
	load_icode(new_env, binary);

	// If this is the file server (type == ENV_TYPE_FS) give it I/O privileges.
	// LAB 5: Your code here.
	if(type == ENV_TYPE_FS) {
		(*new_env).env_tf.tf_eflags |= FL_IOPL_3;
	}
}

//
// Frees env e and all memory it uses.
//
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(PADDR(kern_pgdir));

	// Note the environment's demise.
	cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = PADDR(e->env_pgdir);
	e->env_pgdir = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
//
void
env_destroy(struct Env *e)
{
	// If e is currently running on other CPUs, we change its state to
	// ENV_DYING. A zombie environment will be freed the next time
	// it traps to the kernel.
	if (e->env_status == ENV_RUNNING && curenv != e) {
		e->env_status = ENV_DYING;
		return;
	}

	// 在env_free(e)之前，把e所有的僵死子进程的资源先free掉。
	//
	// 写这几句的本意是：在执行spin.c时我发现父进程执行sys_env_destroy(env);后，子进程被标记为僵死进程，
	// 但还没有env_free子进程，这时父进程执行完成后就自己退出了(也就是destroy + free)，那么就出现了无人
	// 回收子进程内核资源的情况，所以这里补充了这几句。
	for(size_t i = 0; i < NENV; i++) {
		if(envs[i].env_status == ENV_DYING && envs[i].env_parent_id == e->env_id) {
			env_free(&envs[i]);
		}
	}

	env_free(e);

	if (curenv == e) {
		curenv = NULL;
		sched_yield();
	}
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
void
env_pop_tf(struct Trapframe *tf)
{
	// Record the CPU we are running on for user-space debugging
	curenv->env_cpunum = cpunum();

	// 因为执行iret后会从内核态切回用户态继续执行用户代码了，所以这里是释放锁的最晚时机。
	// 读者应该自行检查一下，是否所有的lock_kernel()代码最后都能执行到这里。
	unlock_kernel();

	asm volatile(
		"\tmovl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret\n"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

//
// Context switch from curenv to env e.
// Note: if this is the first call to env_run, curenv is NULL.
//
// This function does not return.
//
void
env_run(struct Env *e)
{
	// Hint: This function loads the new environment's state from
	//	e->env_tf.  Go back through the code you wrote above
	//	and make sure you have set the relevant parts of
	//	e->env_tf to sensible values.

	// LAB 3: Your code here.

	// Note: if this is the first call to env_run, curenv is NULL.
	if(curenv == NULL) {
		// Set 'curenv' to the new environment
		curenv = e;
	}
	// If this is a context switch (a new environment is running):
	else if(curenv->env_id != e->env_id) {
		// Set the current environment (if any) back to ENV_RUNNABLE if
		// it is ENV_RUNNING (think about what other states it can be in)
		if(curenv->env_status == ENV_RUNNING) {
			curenv->env_status = ENV_RUNNABLE;
		}
		curenv = e;
	}
	// Update its 'env_runs' counter
	curenv->env_runs++;

	// 注意!!!
	// Lab 4必须将这两句移动到这里，因为从sched_yield()进来的情况下，
	// 上面的两个if都不会执行！如果不注意到这个细节，那么就enjoy dubugging吧 :)
	curenv->env_status = ENV_RUNNING;	// Set its status to ENV_RUNNING
	lcr3(PADDR(curenv->env_pgdir));	 // Use lcr3() to switch to its address space

	// 实现"谁启动谁完成"的语意，详见sched_yield()的case 2
	curenv->env_cpunum = cpunum();

	// Use env_pop_tf() to restore the environment's
	// registers and drop into user mode in the environment.
	// 执行这个函数后，就会进入到用户态，并执行用户程序
	// 但是我相信你看完env_pop_tf()一定傻眼了，明明没有函数调用call指令，
	// 怎么就能执行用户程序了？？？解释将在这个函数后的注释中给出
	env_pop_tf(&curenv->env_tf);

	//panic("env_run not yet implemented");
}
/*
 * --- env_pop_tf()到底是怎样切换到用户程序执行的？
 *
 * 在传统编程语言里，我们要跳转到某个地方执行，最简单直接也是用的最多的是函数调用，按理来说我们知道了程序的入口，
 * 就可以直接通过函数调用去到那里执行，比如在boot/main.c中跳转到内核的入口执行是这样写的：
 * 
 * 		((void (*)(void)) (ELFHDR->e_entry))();
 * 
 * 但是，这样写不能满足一个极其重要的需求，也就是：
 * 		不支持内核态到用户态的切换。普通的函数调用，不会发生特权级的切换，我们不可能直接在内核态执行用户
 * 		程序，否则用户程序可能危害内核，所以不能直接去调用用户程序。
 * 
 * 所以在这里，你不能直接运行简单的用户程序的函数调用，必须借助其他手段，有没有某种机制能满足上述需求呢？
 * 
 * 还真有，那就是中断机制。回想一下，发生中断时，会从用户态切换到内核态，会把当前指令的下一条指令的地址保存下来，
 * 作为中断返回后继续执行的位置；中断处理完成后，中断返回会从内核态切换到用户态，会根据之前保存下来的指令地址，
 * 跳转到对应的地方继续执行。你想想，只要我们把中断时保存的返回地址“偷偷地”改成我们想要执行的指令的地址，那中断返回后，
 * 是不是就直接跳转到我们想要执行的代码去执行了？并且特权级的转换也由中断指令帮我们自动完成了，不劳我们费心。
 * 
 * 所以，我们借助中断的机制，实现我们从内核跳转到用户代码执行的目的。env_pop_tf()中的最后一条iret就是中断返回指令。
 * 
 * 但是现在还是有个问题，中断机制是配套使用的，应该是由两步组成的: <发生中断><中断返回>，而现在并没有发生中断啊，
 * 怎么能直接调用中断返回指令iret呢？其实硬件并不会检测是否真的发生了中断，当机器看到iret时，他就自动去做中断返回
 * 对应的操作，并不关心是否真的发生了中断。所以，我们只需要人为的模拟，或者说假装发生了中断即可。
 * 
 * 那么现在只需要模拟发生了中断即可。发生中断时机器到底做了哪些操作？最最最简单的就是把当前指令的下一条指令的地址保存
 * 下来，具体来说是保存到栈上面，所以我们只需要把我们想要跳转到的地址放到栈里面就行（实际上不只有返回地址，细节读者可以
 * 自己去了解），于是乎，在load_icode()中有这么一行：
 * 
 * 		e->env_tf.tf_eip = (uintptr_t)elf_hdr->e_entry;
 * 
 * 你可能要说：“欸，这不是放到了Trapframe结构体吗？不是说好要放到栈上面吗？”别急，我们看看env_pop_tf()的输入参数：
 * 
 * 		env_pop_tf(&curenv->env_tf);
 * 
 * 然后看一眼env_pop_tf()的第一行汇编：
 * 
 * 		movl %0,%%esp
 * 
 * 这里面 %0 代表的是函数输入的第一个参数，也就是&curenv->env_tf，而esp就是栈指针寄存器。你看，这不就把栈设置成了
 * 我们的Trapframe了吗？然后iret指令执行时，他会去esp寄存器指向的地方去找返回地址，也就是去Trapframe中找返回地址，
 * 这样就正好找到了elf_hdr->e_entry，并跳转到那里执行。
 * 
 * 总算讲完了，是不是很巧妙呢 :)
 * 
 */
