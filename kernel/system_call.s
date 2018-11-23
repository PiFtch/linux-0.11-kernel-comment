/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 */
/* system_call.s包含系统调用底层处理子程序，由于有些代码比较类似，所以同时也包括时钟中断处理句柄，
硬盘和软盘的中断处理程序也在这里。
这段代码处理信号识别，在每次时钟中断和系统调用之后都会进行识别。一般中断信号并不处理信号识别，因为
会给系统造成混乱。
从系统调用返回(ret_from_system_call)时堆栈的内容见19-31行。 */
SIG_CHLD	= 17	// 定义SIG_CHLD信号（子进程停止或结束）

EAX		= 0x00		// 堆栈中各个寄存器的偏移位置
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C
CS		= 0x20
EFLAGS		= 0x24
OLDESP		= 0x28	# 当有特权级变化时
OLDSS		= 0x2C
# 以下是task_struct中变量的偏移值，include/linux/sched.h
state	= 0		# these are offsets into the task-struct. 进程状态码
counter	= 4		# 任务运行时间计数（递减）（滴答数），运行时间片
priority = 8	# 运行优先数。任务开始运行时counter=priority，越大则运行时间越长
signal	= 12	# 信号位图，每个比特位代表一种信号，信号值=位偏移值+1
sigaction = 16		# MUST be 16 (=len of sigaction)信号执行属性结构数组的偏移值，对应信号将要执行的操作和标志信息。
blocked = (33*16)	# 受阻塞信号位图的偏移量
# 以下定义在sigaction结构中的偏移量，include/signal.h
# offsets within sigaction
sa_handler = 0		# 信号处理过程的句柄
sa_mask = 4			# 信号量屏蔽码
sa_flags = 8		# 信号集
sa_restorer = 12	# 恢复函数指针，linux/signl.c

nr_system_calls = 72	# 0.11内核系统调用总数

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2	# 内存4字节对齐
bad_sys_call:	# 错误的系统调用号从这里返回
	movl $-1,%eax
	iret
.align 2
reschedule:		# 重新执行调度程序入口。调度程序schedule在kernel/sched.c
	pushl $ret_from_sys_call	# 将ret_from_sys_call地址入栈
	jmp _schedule	# 重新执行调度程序
.align 2
_system_call:	# int 0x80--系统调用入口点（调用中断int 0x80，eax中是调用号
	cmpl $nr_system_calls-1,%eax
	ja bad_sys_call
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	movl $0x10,%edx		# set up ds,es to kernel space	全局描述符表中数据段描述符
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx		# fs points to local data space 局部描述符表中的数据段描述符
	mov %dx,%fs
	call _sys_call_table(,%eax,4)	# _sys_call_table + %eax * 4 对应的C程序中的sys_call_table在include/linux/sys.h中，其中定义了一个包括72个系统调用C处理函数的地址数组表。
	pushl %eax			# 系统调用返回值入栈
	movl _current,%eax	# 取当前任务数据结构地址->eax
	# 查看当前任务的运行状态，如果不在就绪状态（state!=0）就去1执行调度程序
	# 如果任务在就绪状态但时间片已经用完（counter==0），也去执行调度程序
	cmpl $0,state(%eax)		# state
	jne reschedule
	cmpl $0,counter(%eax)		# counter
	je reschedule
# 以下代码从系统调用C函数返回后，对信号量进行识别处理，首先判别当前任务是否是初始任务rask0，如果是则不必对其进行信号量方面
# 的处理，直接返回。_task对应C程序中的task[]数组，直接引用task相当于引用task[0]。
ret_from_sys_call:
	movl _current,%eax		# task[0] cannot have signals
	cmpl _task,%eax
	je 3f					# 向前跳转到标号3（forward）
# 通过对原调用程序代码选择符的检查来判断调用程序是否是内核任务（如任务0）。如果是就直接退出中断，否则就需进行信号量的处理。
# 这里比较选择符是否为普通用户代码段的选择符0x000f(RPL=3，局部表，第1个段（代码段）），如果不是则跳转退出中断程序。
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ?
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ? 如果原堆栈段选择符不为0x17（即原堆栈不再用户数据段中），则也退出。
	jne 3f
	movl signal(%eax),%ebx		# signal是task_struct中信号位图偏移量，见文件上部，为12，信号位图共32位，32个信号
	movl blocked(%eax),%ecx		# 取阻塞（屏蔽）信号位图->ecx
	notl %ecx
	andl %ebx,%ecx				# 获得许可的信号位图
	bsfl %ecx,%ecx				# 从低位（位0）开始扫描位图，看是否有1的位，若有则ecx保留该位的偏移值（即第几位0-31）。
	je 3f						# 如果没有信号则向前跳转退出
	btrl %ecx,%ebx				# 复位该信号（ebx含有原signal位图）。
	movl %ebx,signal(%eax)		# 重新保存signal位图信息->current->signal
	incl %ecx					# 将信号调整为从1开始的数（1-32）
	pushl %ecx					# 信号值入栈作为_do_signal的参数之一
	call _do_signal				# 调用C函数信号处理程序，kernel/signal.c
	popl %eax					# 弹出信号值
3:	popl %eax		# 退出中断
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret
# int16--以下代码处理协处理器发出的出错信号。跳转执行C函数math_error() kernel/math/math_emulate.c，
# 返回后将跳转到ret_from_sys_call处继续执行
.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax		# ds、es指向内核数据段
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax		# fs指向局部数据段（出错程序的数据段）
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error		# 执行C函数math_error() kernel/math/math_emulate.c
# int7--设备不存在或协处理器不存在
# 若控制寄存器CR0的EM标志置位，则当CPU执行一个转义指令时就会引发该中断，这样就有机会让这个中断处理程序模拟转义指令。
# CR0的TS标志是在CPU执行任务转换时设置的。TS可以用来确定什么时候协处理器中的内容与CPU正在执行的任务不匹配了。当CPU在运行一个
# 转义指令时发现TS置位了，就会引发该中断。此时就应该恢复新任务的协处理器执行状态。参见kernel/sched.c中的说明，该中断最后
# 将转移到标号ret_from_sys_call处执行（检测并处理信号）。
.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call	# 将下面跳转或调用返回的地址入栈
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore	# 如果不是EM引起的中断，则恢复新任务协处理器状态，执行C函数math_state_restore() kernel/sched.c
	pushl %ebp
	pushl %esi
	pushl %edi
	call _math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret						# 这里的ret将跳转到ret_from_sys_call
# int32--int 0x20时钟中断处理程序。终端频率被设置为100Hz(include/linux/sched.h)，定时芯片8253/8254实在kernel/sched.c中初始化的。
# 因此这里jiffies每10ms加1.这段代码将jiffies增1，发送结束中断指令给8259控制器，然后用当前特权级作为参数调用C函数d0_timer(long CPL)。
# 当调用返回时去检测并处理信号。
.align 2
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
# 由于初始化中断控制芯片时没有采用自动EOI，所以这里需要发指令结束该硬件中断。
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20		# 操作命令字OCW2送0x20端口。
# 下面三局从选择符中取出当前特权级别并压入堆栈，作为do_timer的参数。
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
# do_timer(CPL)执行任务切换、计时等工作，在kernel/sched.c中实现
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call
# sys_execve()系统调用。取中断调用程序的代码指针作为参数调用C函数do_execve()。
# do_execve()在fs/exec.c。
.align 2
_sys_execve:
	lea EIP(%esp),%eax
	pushl %eax
	call _do_execve
	addl $4,%esp		# 丢弃入栈的EIP值
	ret
# sys_fork()调用，用于创建子进程，时system_call功能2.原型在include/linux/sys.h中。
# 首先调用C函数find_empty_process(),取得一个进程号pid。若返回负数则说明目前任务数组
# 已满。然后调用copy_process()复制进程。
.align 2
_sys_fork:
	call _find_empty_process	# kernel/fork.c
	testl %eax,%eax
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process			# kernel/fork.c
	addl $20,%esp				# 丢弃所有的压栈内容
1:	ret
# int46--int 0x2e硬盘中断处理程序，相应硬件中断请求IRQ14.
# 当硬盘操作完成或出错就会发出此中断信号。参见kernel/blk_drv/hd.c.首先项8259A从芯片
# 发送结束硬件中断指令（EOI），然后取变量do_hd中的函数指针放入edx寄存器中，并置do_hd
# 为NULL，接着判断edx函数指针是否为空。如果为空，给edx赋值指向unexpected_hd_interrupt(),
# 用于显示出错信息。随后项8259A主芯片送EOI指令，并调用edx中指针指向的函数:read_intr()、write_intr()或unexpected_hd_interrupt()。
_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
# 由于初始化中断控制芯片时没有采用自动EOI，所以这里需要发指令结束该硬件中断。
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1 送从8259A
	jmp 1f			# give port chance to breathe
1:	jmp 1f				# 延时作用
1:	xorl %edx,%edx
	xchgl _do_hd,%edx	# do_hd是函数指针，将被赋值read_intr()或write_intr()函数地址blk_drv/hd.c放到edx后就将do_hd置为NULL。
	testl %edx,%edx		# 测试函数指针是否为NULL。
	jne 1f				# 若为NULL，则使指针指向C函数unexpected_hs_interrupt()。
	movl $_unexpected_hd_interrupt,%edx	# kernel/blk_drv/hd.c
1:	outb %al,$0x20		# 送主8259A EOI指令（结束硬件中断）
	call *%edx		# "interesting" way of handling intr.	调用do_hd指向的C函数。
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
# int38--int 0x26软盘驱动器中断处理程序，响应硬件中断请求IRQ6。
# 处理过程与硬盘基本相同kernel/blk_drv/floppy.c。首先向8259A从芯片发送EOI指令，然后去变量do_floppy中函数指针放入eax寄存器中，
# 并置do_floppy为NULL,接着判断eax是否为空，如为空，则给eax赋值，使其指向unexpected_floppy_interrupt()，用于显示出错信息。
# 随后调用eax指向的函数:rw_interrupt、seek_interrupt、recal_interrupt、reset_interrupt或unexpected_floppy_interrupt。
_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax	# 放到eax后_do_floppy指针变量置空。
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr. 调用_do_floppy指向的函数。
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
# int39--int 0x27并行口中断处理程序，对应硬件中断请求信号IRQ7。
_parallel_interrupt:	# 本版本内核未实现，这里只是发送EOI指令。
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
