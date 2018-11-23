/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */
# 本代码文件主要设计对intel保留中断0-16的处理，以下是一些全局函数的生命，原型在traps.c中
.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_coprocessor_error,_irq13,_reserved

_divide_error:
	pushl $_do_divide_error		# 首先把将要调用的函数地址入栈，这段程序的出错号为0
no_error_code:					# 无出错号处理的入口处
	xchgl %eax,(%esp)			# do_divide_error的地址与eax交换，eax入栈
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds					# 16位入栈也占用4B
	push %es
	push %fs
	pushl $0		# "error code"
	lea 44(%esp),%edx			# 取原调用返回地址处堆栈指针位置，入栈
	pushl %edx
	movl $0x10,%edx				# 内核代码数据段选择符
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	call *%eax					# 调用C函数do_divide_error()
	addl $8,%esp				# 让堆栈指针重新指向寄存器fs入栈处
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax					# 弹出原来eax中的内容
	iret

_debug:			# int1--debug调试中断入口点
	pushl $_do_int3		# _do_debug C函数指针入栈，以下同
	jmp no_error_code

_nmi:			# int2--非屏蔽中断入口点
	pushl $_do_nmi
	jmp no_error_code

_int3:			# 断点指令引起中断的入口点，处理过程同_debug
	pushl $_do_int3
	jmp no_error_code

_overflow:		# 溢出出错处理中断入口点
	pushl $_do_overflow
	jmp no_error_code

_bounds:		# int5--边界检查出错中断入口点
	pushl $_do_bounds
	jmp no_error_code

_invalid_op:	# int6--无效操作指令出错中断入口点
	pushl $_do_invalid_op
	jmp no_error_code

_coprocessor_segment_overrun:	# int9--协处理器段超出出错中断入口点
	pushl $_do_coprocessor_segment_overrun
	jmp no_error_code

_reserved:		# int15--保留
	pushl $_do_reserved
	jmp no_error_code
# 下面用于当协处理器执行完一个操作时就会发出IRQ13中断信号，以通知CPU操作完成
_irq13:			# int45--（(0x20+13)数学协处理器发出的中断
	pushl %eax
	xorb %al,%al	# 80387在执行计算时，CPU会等待其操作的完成
	outb %al,$0xF0	# 通过写0XF0端口，本中断消除cpu的BUSY延续信号，并重新激活387的处理器扩展请求引脚PEREQ。该操作主要是为了确保在继续执行387的任何指令之前，响应本中断
	movb $0x20,%al
	outb %al,$0x20	# 向8259主中断控制芯片发送EOI（中断结束）信号
	jmp 1f			# 这两个跳转指令起延时作用
1:	jmp 1f
1:	outb %al,$0xA0	# 再向8259从芯片发送EOI信号
	popl %eax
	jmp _coprocessor_error	# kernel/system_call.s
# 以下中断在调用时会在中断返回地址之后将出错号压入堆栈，因此返回时也需要将出错号弹出
# int8--双出错故障
_double_fault:
	pushl $_do_double_fault	# C函数地址入栈
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax	eax原来的值被保存在堆栈上
	xchgl %ebx,(%esp)		# &function <-> %ebx	ebx原来的值被保存在堆栈上
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code
	lea 44(%esp),%eax		# offset	程序返回地址处堆栈指针位置入栈
	pushl %eax
	movl $0x10,%eax		# 置内核段选择符
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	call *%ebx			# 调用相应的C函数，其参数已入栈
	addl $8,%esp		# 堆栈指针重新指向栈中放置fs内容的位置
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

_invalid_TSS:			# int10--无效的任务状态段TSS
	pushl $_do_invalid_TSS
	jmp error_code

_segment_not_present:	# int11--段不存在
	pushl $_do_segment_not_present
	jmp error_code

_stack_segment:			# int12--堆栈段错误
	pushl $_do_stack_segment
	jmp error_code

_general_protection:	# int13--一般保护性出错
	pushl $_do_general_protection
	jmp error_code
# int7--设备不存在，kernel/system_calls.s
# int14--页错误，在mm/page.s
# int 16--协处理器错误,在system_calls.s
# 时钟中断int 0x20和系统调用int 0x80在system_call.s