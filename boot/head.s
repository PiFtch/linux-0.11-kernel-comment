/*
 *  linux/boot/head.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  head.s contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00000000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory.
 */
.text
.globl _idt,_gdt,_pg_dir,_tmp_floppy_area
_pg_dir:
startup_32:
	movl $0x10,%eax		// ax：0000 0000 0001 0000
	mov %ax,%ds			// 0-1位标识特权级0，2位为0表示选择全局描述符表，3-15位为2，表示选择表中第二项
	mov %ax,%es			// 见setup.s，GDT第二项为数据段描述符
	mov %ax,%fs
	mov %ax,%gs
	lss _stack_start,%esp // _stack_start位于kernel/sched.c, 地址高值送SS，低值送esp
	call setup_idt		// 设置IDT
	call setup_gdt		// 设置GDT
	movl $0x10,%eax		# reload all the segment registers
	mov %ax,%ds		# after changing gdt. CS was already
	mov %ax,%es		# reloaded in 'setup_gdt'
	mov %ax,%fs		// 由于修改了GDT，重新装在所有段寄存器，cs已经在setup_gdt中装载过了
	mov %ax,%gs
	lss _stack_start,%esp
	xorl %eax,%eax	// 测试A20地址线是否已经开启，方法是项内存地址0x000000处写入任意一个数值，然后看内存地址0x100000（1M）处是否也是这个数值，如果一直相同的话，就一直比较下去，造成死循环，表示A20没有选通，内核不能使用1MB以上内存
1:	incl %eax		# check that A20 really IS enabled
	movl %eax,0x000000	# loop forever if it isn't
	cmpl %eax,0x100000
	je 1b			// 1b表示向后跳转至标号1，若为5f则表示向前跳转至标号5
/*
 * NOTE! 486 should set bit 16, to check for write-protect in supervisor
 * mode. Then it would be unnecessary with the "verify_area()"-calls.
 * 486 users probably want to set the NE (#5) bit also, so as to use
 * int 16 for math errors.
 */ // 检查数学协处理器芯片是否存在，通过修改控制寄存器CR0，在假设存在的情况下执行一个协处理器指令，出错则说明不存在，需要置CE0中的协处理器仿真位EM（位2），并复位协处理器存在标志MP（位1）
	movl %cr0,%eax		# check math chip
	andl $0x80000011,%eax	# Save PG,PE,ET
/* "orl $0x10020,%eax" here for 486 might be good */
	orl $2,%eax		# set MP
	movl %eax,%cr0
	call check_x87
	jmp after_page_tables

/*
 * We depend on ET to be correct. This checks for 287/387.
 */
check_x87:
	fninit
	fstsw %ax
	cmpb $0,%al
	je 1f			/* no coprocessor: have to set bits */
	movl %cr0,%eax		// 如果存在则向前跳转到1，否则改写CR0
	xorl $6,%eax		/* reset MP, set EM */
	movl %eax,%cr0
	ret
.align 2			// 存储边界对齐，2表示调整到地址最后2位为0，即按4字节方式对齐内存地址
1:	.byte 0xDB,0xE4		/* fsetpm for 287, ignored by 387 */
	ret

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
	lea ignore_int,%edx
	movl $0x00080000,%eax	// eax高16位置0x0008
	movw %dx,%ax		/* selector = 0x0008 = cs */ // 此时eax含有门描述符低4B的值
	movw $0x8E00,%dx	/* interrupt gate - dpl=0, present */ // edx含有门描述符高4B的值

	lea _idt,%edi		// _idt为中断描述符表的地址 lea传送偏移地址
	mov $256,%ecx
rp_sidt:
	movl %eax,(%edi)	// 将哑中断门描述符存入idt表中
	movl %edx,4(%edi)	
	addl $8,%edi		// edi指向idt表中下一项
	dec %ecx			// 填满256个表项，使其都指向ignore_int中断门
	jne rp_sidt
	lidt idt_descr		// 加载IDTR的值
	ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will beoverwritten by the page tables.
 */
setup_gdt:
	lgdt gdt_descr
	ret

/*
 * I put the kernel page tables right after the page directory,
 * using 4 of them to span 16 Mb of physical memory. People with
 * more than 16MB will have to expand this.
 */
/*
每个页表长4KB，每个页表项需要4B，一个页表共可存放1024个表项，如果一个表项寻址4KB地址空间，则一个页表就可以寻址4MB物理内存。
页表项格式为：前0-11位存放标志，如是否在内存中（P位0），读写许可（R/W位1），普通/超级用户（U/S位2），是否修改过（D位6）等；
位12-31为页框地址，用于指出一页内存的物理起始地址
*/
.org 0x1000 // 从偏移0x1000处开始是第一个页表，偏移0处开始存放也表目录
pg0:

.org 0x2000
pg1:

.org 0x3000
pg2:

.org 0x4000
pg3:

.org 0x5000	// 以下内存数据块从偏移0x5000处开始
/*
 * tmp_floppy_area is used by the floppy-driver when DMA cannot
 * reach to a buffer-block. It needs to be aligned, so that it isn't
 * on a 64kB border.
 */ // 直接存储器访问不能访问缓冲块时，以下内存块就可供软盘驱动程序使用，其地址需要对齐调整，这样就不会跨越64KB边界
_tmp_floppy_area:
	.fill 1024,1,0
/*
为调用init/main.c和返回做准备。
push $L6模拟调用main.c时首先将返回地址入栈的操作，如果main.c程序真正退出时，会返回到标号L6继续执行，即死循环。
下一行将main.c程序的地址压入堆栈，在setup_paging结束后执行ret就会将main.c的地址弹出堆栈，病区执行main.c
*/
after_page_tables:
	pushl $0		# These are the parameters to main :-)
	pushl $0
	pushl $0
	pushl $L6		# return address for main, if it decides to.
	pushl $_main
	jmp setup_paging
L6:
	jmp L6			# main should never return here, but
				# just in case, we know what happens.

/* This is the default interrupt "handler" :-) */
int_msg:
	.asciz "Unknown interrupt\n\r"
.align 2
ignore_int:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds		// ds es fs gs等虽然是16位寄存器，但仍会以32位形式入栈，即需要占用4个字节的堆栈空间
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds		// ds es fs指向GDT表中的数据段
	mov %ax,%es
	mov %ax,%fs
	pushl $int_msg	// 把调用printk函数的参数指针（地址）入栈
	call _printk	// _printk是printk编译后模块中的内部表示法
	popl %eax
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret			// 中断返回（把中断调用时压入栈的CPU标志寄存器（32位）值也弹出）。


/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 16MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 16 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 16Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "16Mb"), but I
 * won't guarantee that's all :-( )
 */
.align 2	// 四字节对齐
setup_paging:
	movl $1024*5,%ecx		/* 5 pages - pg_dir+4 page tables */
	xorl %eax,%eax
	xorl %edi,%edi			/* pg_dir is at 0x000 */
	cld;rep;stosl			// es:edi
	// 设置页目录的项，共有四个页表所以只需设置四个项。 第一个页表所在1地址为0x00001007 & 0xfffff000 = 0x1000, 标志为0x00001007 & 0x00000fff = 0x07,该页存在，用户可读写
	movl $pg0+7,_pg_dir		/* set present bit/user r/w */ // 0x00001007 _pg_dir位于head/开头，因此位于0x00000000
	movl $pg1+7,_pg_dir+4		/*  --------- " " --------- */ // 每项4B
	movl $pg2+7,_pg_dir+8		/*  --------- " " --------- */
	movl $pg3+7,_pg_dir+12		/*  --------- " " --------- */
	/*
	填写四个页表中所有项的内容。共4*1024=4096项，可映射4096*4KB=16MB物理内存
	*/
	movl $pg3+4092,%edi		// 最后一页的最后一项
	movl $0xfff007,%eax		/*  16Mb - 4096 + 7 (r/w user,p) */
	std				// 方向位置位，edi递减4B
1:	stosl			/* fill pages backwards - more efficient :-) */
	subl $0x1000,%eax	// 填好一项，就将eax-0x1000，使其指向前一个页面物理地址
	jge 1b				// 大于0，还有项未填，回1，否则全部填好
	xorl %eax,%eax		/* pg_dir is at 0x0000 */
	movl %eax,%cr3		/* cr3 - page directory start */ // cr3-页目录基址寄存器
	movl %cr0,%eax		// 设置启动使用分页处理(CR0的PG标志，位31)
	orl $0x80000000,%eax
	movl %eax,%cr0		/* set paging (PG) bit */
	ret			/* this also flushes prefetch-queue */
/* 改变分页处理标志后，要求使用转移指令刷新预取指令队列，这里使用ret。它的另一个作用是将堆栈中的main程序的地址弹出，并开始运行init/main.c程序。 */
.align 2
.word 0
idt_descr:				// 低位16位限制256，高32位idt基址
	.word 256*8-1		# idt contains 256 entries
	.long _idt
.align 2
.word 0
gdt_descr:				// 16位限制，32位基址
	.word 256*8-1		# so does gdt (not that that's any
	.long _gdt		# magic number, but it works for me :^)

	.align 3
_idt:	.fill 256,8,0		# idt is uninitialized 256项，每项8字节，填0
/* 全局表，前四项分别是空项，代码段描述符，数据段描述符，系统段描述符（不使用），预留252项用于放置所创建任务的LDT和对应的任务状态段TSS的描述符(0-nul,1-cs,2-ds,3-sys,4-TSS0,
   5-LDT0,6-TSS1,7-LDT1,8-TSS2,etc...) */
_gdt:	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x00c09a0000000fff	/* 16Mb */	// 代码段最大长度16Mb
	.quad 0x00c0920000000fff	/* 16Mb */	// 数据段
	.quad 0x0000000000000000	/* TEMPORARY - don't use */
	.fill 252,8,0			/* space for LDT's and TSS's etc */
