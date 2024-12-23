// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	unsigned pn = ((uint32_t)addr) / PGSIZE; 
	if((err & FEC_WR) != FEC_WR || (uvpt[pn] & PTE_COW) != PTE_COW) {
		panic("pgfault: the faulting is not a write or not PTE_COW\n");
	}

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
	/*
	 * 为什么这里不能直接使用thisenv->env_id？答案在函数后的注释中给出
	 */
	envid_t cur_eid = sys_getenvid();

	if((r = sys_page_alloc(cur_eid, PFTEMP, PTE_P | PTE_U | PTE_W)) < 0)
		panic("pgfault: sys_page_alloc: %e", r);
	memmove(PFTEMP, (void*)(pn * PGSIZE), PGSIZE);
	if((r = sys_page_map(cur_eid, PFTEMP, cur_eid, (void*)(pn * PGSIZE), PTE_P | PTE_U | PTE_W)) < 0)
		panic("pgfault: sys_page_map: %e", r);
	if((r = sys_page_unmap(cur_eid, PFTEMP)) < 0)
		panic("pgfault: sys_page_unmap: %e", r);

	//panic("pgfault not implemented");
}
/*
 * ---------- 为什么这里不能直接使用thisenv->env_id？
 *
 * 因为当子进程可以运行时，它是从fork()中的envid = sys_exofork();这句话开始执行的，
 * 在它执行到thisenv = &envs[ENVX(sys_getenvid())];之前，必定会访问到用户栈空间，
 * 而这时用户栈空间是COW的，所以会触发page fault，进而执行到pgfault()函数。如果pgfault()
 * 中用的是thisenv->env_id (显然这个时候的thisenv是错误的值)，就会出问题。
 */

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
	//panic("duppage not implemented");
	uint32_t va = pn * PGSIZE;
	pte_t cur = uvpt[pn];
	int perm = cur & 0x0fff;
	// If the page is writable or copy-on-write
	if((perm & PTE_W) == PTE_W || (perm & PTE_COW) == PTE_COW) {
		//cprintf("cow page: %d\n", pn);
		if((r = sys_page_map(thisenv->env_id, (void*)va, envid, (void*)va, PTE_U | PTE_P | PTE_COW)) < 0)
			panic("duppage: sys_page_map: %e", r);
		// remap
		if((r = sys_page_map(thisenv->env_id, (void*)va, thisenv->env_id, (void*)va, PTE_U | PTE_P | PTE_COW)) < 0)
			panic("duppage: sys_page_map: %e", r);
	}
	else {
		if((r = sys_page_map(thisenv->env_id, (void*)va, envid, (void*)va, perm)) < 0)
			panic("duppage: sys_page_map: %e", r);
	}

	return 0;
}

static void
copy_address_space(envid_t envid)
{
	// 遍历页目录表项
	for(size_t i = 0; i < 1024; i++) {
		bool complete_flag = false;
		if(uvpd[i] != 0) {
			// 对于每个非空的页目录表项，遍历其所有的页表项
			for(size_t j = 0; j < 1024; j++) {
				unsigned pn = i * 1024 + j;
				uint32_t va = pn * PGSIZE;
				// 只需要遍历到USTACKTOP即可，因为User Exception Stack不能共享
				if(va >= USTACKTOP) {
					complete_flag = true;
					break;
				}
				if(uvpt[pn] != 0) {
					duppage(envid, pn);
				}
			}
		}
		if(complete_flag) {
			break;
		}
	}

	// 用户异常栈需要独立分配，不能是COW
	int r;
	if((r = sys_page_alloc(envid, (void*)(UXSTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) < 0)
		panic("copy_address_space: sys_page_alloc: %e", r);
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
	//panic("fork not implemented");
	// 这个函数大体逻辑参照dumbfork()即可

	// 1. Set up our page fault handler appropriately.
	set_pgfault_handler(pgfault);

	// 2. Create a child.
	envid_t envid;
	envid = sys_exofork();
	if (envid < 0)
		panic("fork: sys_exofork: %e", envid);
	if (envid == 0) {
		// We're the child.
		// The copied value of the global variable 'thisenv'
		// is no longer valid (it refers to the parent!).
		thisenv = &envs[ENVX(sys_getenvid())];	// Remember to fix "thisenv" in the child process.
		return 0;
	}

	// 3. Copy our address space and page fault handler setup to the child.
	/*
	 * fork()中最难理解的就是uvpd, uvpt的设计原理，且听我在函数后的注释中详细解释
	 */
	copy_address_space(envid);
	/*
	 * 读者可以想一下，为什么这里只需要设置env_pgfault_upcall即可，而不需要设置_pgfault_handler？
	 */
	if(sys_env_set_pgfault_upcall(envid, thisenv->env_pgfault_upcall) != 0) {
		panic("fork: sys_env_set_pgfault_upcall fail\n");
	}

	// 4. Then mark the child as runnable and return.
	int r;
	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
		panic("fork: sys_env_set_status: %e", r);

	return envid;
}
/*
 * ----------- uvpd, uvpt的设计原理
 *
 * 请先详细阅读官方解释：https://pdos.csail.mit.edu/6.828/2018/labs/lab4/uvpt.html
 * 
 * 一般情况下，页目录表和页表应该都是对用户不可见的，但是在某些情况下，我们还是希望能访问到它们，
 * 比如这里的duppage()，我们需要知道当前的页表项对应的页表是否是可写的或者COW的。所以我们首先会
 * 有这样一个需求，就是：能便捷地访问到页目录表项和页表项。
 * 
 * 那么回想一下，以往我们是怎么访问页表项的呢？是通过pgdir_walk()！但是显然，每次都要调用这么一个
 * 复杂的函数太麻烦了，而且pgdir_walk()这么核心的函数如果开放出来，是不是有点不太安全？所以我们
 * 需要设计另外一种方式，实现我们的需求。
 * 
 * 所以就有了uvpd, uvpt这样一种方式。
 * 
 * 在我们看来，其实无论是页目录表还是页表，本质上不就是一个数组吗？如果我们能像使用数组那样便捷地访问
 * 到页目录表项和页表项，不就很好吗？比如uvpd[0]就是获取页目录表的第一个页目录表项。
 * 
 * 怎么实现呢？这就要先重温一下mmu的运行原理了。在处理器的眼中，地址的转换就是三条指令：
 * 		pd = lcr3(); 			// 1. 从cr3寄存器中获取页目录表的首地址
 * 		pt = *(pd+4*PDX); 		// 2. 通过(页目录表首地址 + PDX偏移)的方式获取到页表的首地址
 * 		page = *(pt+4*PTX);		// 3. 通过(页表首地址 + PTX偏移)的方式获取到物理页首地址
 * 
 * 如果，我们在第2步中获取到的不是"页表的首地址"，而是"页目录表首地址"，也就是这样：
 * 		2. (页目录表首地址 + PDX偏移) ----> 页目录表首地址
 * 那么我们在第3步时，是不是实际执行的就是(页目录表首地址 + PTX偏移)吗？
 * 这时，只要我们让 PTX偏移 = PDX偏移 ，那么第3步的执行结果，也必然还是"页目录表首地址"！
 * 也就是说，经过mmu的转换后，我们最终得到的就是"页目录表首地址"！这时就相当于得到一个页目录表数组的首指针，
 * 自然就可以像使用数组一样去访问每一个页目录表项了！
 * 
 * 所以关键就是让第2步返回"页目录表首地址"而不是"页表首地址"，怎么实现呢？回想起env.c中的env_setup_vm()，
 * 其实也就一句话：
 * 		e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;
 * 
 * 所以，如果我们将一个32位的虚拟地址构建为：
 * 		+--------10------+-------10-------+---------12----------+
 * 		|    PDX(UVPT)   |   PDX(UVPT)    |      数组的偏移      |
 *		+----------------+----------------+---------------------+
 * 就可以通过改变数组的偏移快速获取对应的页目录表项。
 * 
 * 至于页表项的快速获取，原理是一样的，就交给读者自己去理解了！
 * 
 * 
 * ----------- 为什么只需要设置env_pgfault_upcall，而不需要设置_pgfault_handler？
 *
 * 因为_pgfault_handler是全局变量，在Program Data & Heap中，这部分在copy_address_space(envid);
 * 中已经让子进程建立起对应的映射关系了，所以没必要特意设置。那为什么env_pgfault_upcall需要单独设置呢？
 * 因为env_pgfault_upcall在UTOP之上，这部分我们可没有建立映射。
 */

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
