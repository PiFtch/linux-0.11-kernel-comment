/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

/**
 * 刷新页变换告诉缓冲宏函数
 * 为提高地址转换效率，CPU将最近使用的页表数据存放在芯片中高速缓冲中。
 * 在修改过页表信息后，就需要刷新该缓冲区。这里使用重新加载页目录基址
 * 寄存器CR3的方法来尽心刷新。eax=0为页目录的基址
 * */
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000
#define PAGING_MEMORY (15*1024*1024)
#define PAGING_PAGES (PAGING_MEMORY>>12)
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)	// 指定的内存地址映射为页号
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")
// 内存映射字节图（1B代表1页），每个页面对应的字节用于标识页面当前被引用（占用）次数。
static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");		// ax initialized as 0

__asm__("std ; repne ; scasb\n\t"		// compare es:di with al(0)		std -- discrement direction
	"jne 1f\n\t"						// if not 0(no empty page), jmp to end
	"movb $1,1(%%edi)\n\t"				// set empty page as used
	"sall $12,%%ecx\n\t"				// PAGING_PAGES << 12
	"addl %2,%%ecx\n\t"					// + LOW_MEM = physical start address
	"movl %%ecx,%%edx\n\t"				
	"movl $1024,%%ecx\n\t"				// rep's count
	"leal 4092(%%edx),%%edi\n\t"
	"rep ; stosl\n\t"					// 将此页面清零
	"movl %%edx,%%eax\n"				// 返回页面起始地址
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)		// edi point to the last page's mem_map
	:"di","cx","dx");
return __res;
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;				// get page number
	if (mem_map[addr]--) return; // if page's used, set as unused and return
	mem_map[addr]=0;			 // else restore mem_map[addr]
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
/**
 * 释放页表指向的内存块并置表项空闲
 * 页目录位于物理地址0开始处，共1024项，4KB
 * from -- 起始基地址；size -- 释放的长度(页表个数)
 * */
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)		// only process 4MB's block(page_table)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	size = (size + 0x3fffff) >> 22;			// 计算所占页目录项数
	// 计算起始目录项
	// 目录项号>>22，每个项号对应页目录表中一个4B的项，因而>>20（页目录表从位置0开始，每项4B）
	// 0xffc使得dir指向页目录项
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {				// size是需要释放的目录项数
		if (!(1 & *dir))					// 最低位表示是否可用于地址转换过程
			continue;						// 不可用于地址转换过程，跳过
		pg_table = (unsigned long *) (0xfffff000 & *dir);	// 取表项高20位的页框，*dir为页目录项，pg_table现在是这个页目录项指向的页表地址
		for (nr=0 ; nr<1024 ; nr++) {		// 每个页表有1024项
			if (1 & *pg_table)				// 页目录表的项若P位为1， 释放对应内存页
				free_page(0xfffff000 & *pg_table);	// 参数为该页目录项中的页框
			*pg_table = 0;					// 该页表项内容清零
			pg_table++;						// 下一项
		}
		free_page(0xfffff000 & *dir);		// 释放该页目录项指向页表的空间
		*dir = 0;							// 该页目录项清零
	}
	invalidate();							// 刷新页变换高速缓冲
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
/**
 * 复制指定线性地址和长度（页表个数）内存对应的页目录项和页表，从而被复制的页目录和页表
 * 对应的原物理内存区被共享使用。
 * 复制指定地址和长度的内存对应的页目录项和页表项，需申请页面来存放新页表，原内存区被共享；
 * 此后两个进程将共享内存区，直到有一个进程执行写操作时，才分配新的内存页（写时复制机制）。
 * */
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;
	// 源和目的地址必须在4MB内存边界上，否则死机
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	// from_dir指向from地址对应的页目录项
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	// to_dir指向to地址对应的页目录项
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	// size -- 要复制的内存块占用的页表数（即页目录项数）
	size = ((unsigned) (size+0x3fffff)) >> 22;
	// 对每个占用的页表依次进行复制操作
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)	// 目的目录项指定的页表已经存在（P=1），则死机
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))	// 源目录项未使用，则跳过
			continue;
		// from_page_table指向from_dir对应页目录项对应的页表基地址
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page()))	// 若返回0，说明没有申请到空闲页面，返回-1并退出
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7;		// 页目录项最低3位置1
		// 针对当前处理的页表，设置需复制的页面数。如果在内核空间，仅需复制头160页，否则需要复制一个页表中所有1024个页面。
		nr = (from==0)?0xA0:1024;
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;	// this_page -- 页表项内容
			if (!(1 & this_page))			// 源页面未使用，则不用复制
				continue;
			/**
			 * 复位页表项中的R/W标志（置0）。（如果U/S位是0，则RW位就没有作用。
			 * 如果US是1，RW是0，那么运行在用户层的代码就只能读页面。如果两个都置位，就有写的权限
			 * */
			this_page &= ~2;
			*to_page_table = this_page;		// 将该页表项复制到目的页表项中
			// 如果该页表项所指页面在1MB以上，则需要设置内存页面映射数组
			if (this_page > LOW_MEM) {
				/**
				 * 令源页表项所指内存页也为只读，因为有两个进程共用内存区了。若其中一个内存需要进行写操作，
				 * 则可以通过页异常的写保护处理，为执行写操作的进程分配一页新的空闲页面，即进行写时复制的操作。
				 * */
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate();		// 刷新页变换高速缓冲
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
/**
 * 将一物理内存页面映射到指定的线性地址处。
 * 主要工作是在页目录和页表中设置指定页面的信息，若成功则返回页面地址。
 * */
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);		// page_table指向页目录项
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);	// page_table为页表基地址
	else {	// 目录项无效（指定的页表不在内存中），申请空闲页面给页表使用，并在对应目录项中置相应标志，将该页表地址保存在page_table中
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;	// 在页表中设置指定地址的物理内存页面的页表项内容
/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	address &= 0xfffff000;
	tmp = address - current->start_code;	// 地址相对于进程基址的偏移长度值
	// 若当前进程executable空，或指定地址超出代码+数据长度，则申请一页物理内存，并映射到指定线性地址处
	// executable是进程i节点结构，该值为0表明进程刚开始设置，需要内存；指定地址超出代码+数据长度，表明进程在申请新的内存空间
	// start_code是进程代码段地址，end_data是代码+数据长度。 对于linux，代码段和数据段起始基址是相同的
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	if (share_page(tmp))	// 如果尝试共享页面成功，则退出
		return;
	if (!(page = get_free_page()))	// 取空闲页面，如果内存不够，调用oom
		oom();
/* remember that 1 block is used for header */	// 程序头使用一个数据块
	block = 1 + tmp/BLOCK_SIZE;		// 计算缺页所在的数据块项。BLOCKSIZE=1024B，因此一页需要4个数据块
	for (i=0 ; i<4 ; block++,i++)	// 根据i节点信息，取数据块在设备上对应的逻辑块号
		nr[i] = bmap(current->executable,block);
	bread_page(page,current->executable->i_dev,nr);	// 读设备上一个页面的数据（4块）到指定物理地址page处
	// 增加1页内存后，该页内存的部分可能超过end_data位置。以下循环对物理页面超出的部分进行清零处理
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	// 如果物理页面映射到指定线性地址的操作成功，就返回，否则释放内存也，显示内存不够
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}
/**
 * 物理内存初始化。start_mem可用作分页处理的物理内存起始位置（已去除RAMDISK所占空间等）；
 * end_mem实际物理内存最大地址。0.11最多使用16MB内存，大于部分弃之不用。
 * 其中，0-1MB用于内核系统（实际是0-640KB）。
 * */
void mem_init(long start_mem, long end_mem)	// 参数为主内存区开始处和末端
{
	int i;

	HIGH_MEMORY = end_mem;	// 设置内存最高端
	for (i=0 ; i<PAGING_PAGES ; i++)	// 置所有页面为已占用状态
		mem_map[i] = USED;	
	i = MAP_NR(start_mem);	// 计算可使用起始内存的页面号
	end_mem -= start_mem;   // 计算可分页处理的内存块大小
	end_mem >>= 12;         // 计算出可用于分页处理的页面数
	while (end_mem-->0)     // 最后将这些可用页面对应的页面映射数组清零
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
