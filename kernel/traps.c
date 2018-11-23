/*
 *  linux/kernel/traps.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'asm.s'. Currently mostly a debugging-aid, will be extended
 * to mainly kill the offending process (probably by giving it a signal,
 * but possibly by killing it outright if necessary).
 */
#include <string.h>

#include <linux/head.h>		// 段描述符结构，选择符常量
#include <linux/sched.h>	// task_struct和初始任务0的数据
#include <linux/kernel.h>	// 内核常用函数的原型定义
#include <asm/system.h>		// 设置或修改描述符/中断门等的嵌入式汇编宏
#include <asm/segment.h>	// 段操作头文件，定义了有关段寄存器操作的嵌入式汇编函数
#include <asm/io.h>			// 定义硬件端口输入输出宏汇编语句
/**
 * 以下语句定义了三个嵌入式汇编宏语句函数。取段seg中地址addr处的一个字节。
 * 用圆括号括住的组合语句（花括号中的语句）可以作为表达式使用，其中最后的__res是其输出值。
 * 嵌入最后部分:输出寄存器 :输入寄存器 :修改的寄存器
 * =表示输出寄存器，a为加载代码，表示eax，类似还有b等，详情gnu gcc手册，0表示使用与上一个相同的寄存器
 * */
#define get_seg_byte(seg,addr) ({ \
register char __res; \
__asm__("push %%fs;mov %%ax,%%fs;movb %%fs:%2,%%al;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})
// 取段seg中地址addr处的一个双字
#define get_seg_long(seg,addr) ({ \
register unsigned long __res; \
__asm__("push %%fs;mov %%ax,%%fs;movl %%fs:%2,%%eax;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})
// 取fs段寄存器的值（选择符）
#define _fs() ({ \
register unsigned short __res; \
__asm__("mov %%fs,%%ax":"=a" (__res):); \
__res;})
// 函数原型
int do_exit(long code);		// 程序退出处理

void page_exception(void);	// 页异常，page_fault
// 一些中断处理程序原型，代码在kernel/asm.s或system_call.s中
void divide_error(void);
void debug(void);
void nmi(void);
void int3(void);
void overflow(void);
void bounds(void);
void invalid_op(void);
void device_not_available(void);
void double_fault(void);
void coprocessor_segment_overrun(void);
void invalid_TSS(void);
void segment_not_present(void);
void stack_segment(void);
void general_protection(void);
void page_fault(void);
void coprocessor_error(void);
void reserved(void);
void parallel_interrupt(void);
void irq13(void);
/**
 * 该子程序用来打印出错中断的名称/出错号/调用程序的EIP,EFLAGS,ESP,FS段寄存器值、段的基址、
 * 段的长度、进程号pid、任务号、10字节指令码。如果堆栈在用户段，则还打印16字节的堆栈内容。
 * */
static void die(char * str,long esp_ptr,long nr)
{
	long * esp = (long *) esp_ptr;
	int i;

	printk("%s: %04x\n\r",str,nr&0xffff);
	printk("EIP:\t%04x:%p\nEFLAGS:\t%p\nESP:\t%04x:%p\n",
		esp[1],esp[0],esp[2],esp[4],esp[3]);
	printk("fs: %04x\n",_fs());
	printk("base: %p, limit: %p\n",get_base(current->ldt[1]),get_limit(0x17));
	if (esp[4] == 0x17) {
		printk("Stack: ");
		for (i=0;i<4;i++)
			printk("%p ",get_seg_long(0x17,i+(long *)esp[3]));
		printk("\n");
	}
	str(i);		// 取当前运行任务的任务号 include/linux/sched.h
	printk("Pid: %d, process nr: %d\n\r",current->pid,0xffff & i);
	for(i=0;i<10;i++)
		printk("%02x ",0xff & get_seg_byte(esp[1],(i+(char *)esp[0])));
	printk("\n\r");
	do_exit(11);		/* play segment exception */
}
// 以下do开头函数是对应名称中断处理程序调用的C函数。
void do_double_fault(long esp, long error_code)
{
	die("double fault",esp,error_code);
}

void do_general_protection(long esp, long error_code)
{
	die("general protection",esp,error_code);
}

void do_divide_error(long esp, long error_code)
{
	die("divide error",esp,error_code);
}

void do_int3(long * esp, long error_code,
		long fs,long es,long ds,
		long ebp,long esi,long edi,
		long edx,long ecx,long ebx,long eax)
{
	int tr;

	__asm__("str %%ax":"=a" (tr):"0" (0));		// 取任务寄存器值->tr
	printk("eax\t\tebx\t\tecx\t\tedx\n\r%8x\t%8x\t%8x\t%8x\n\r",
		eax,ebx,ecx,edx);
	printk("esi\t\tedi\t\tebp\t\tesp\n\r%8x\t%8x\t%8x\t%8x\n\r",
		esi,edi,ebp,(long) esp);
	printk("\n\rds\tes\tfs\ttr\n\r%4x\t%4x\t%4x\t%4x\n\r",
		ds,es,fs,tr);
	printk("EIP: %8x   CS: %4x  EFLAGS: %8x\n\r",esp[0],esp[1],esp[2]);
}

void do_nmi(long esp, long error_code)
{
	die("nmi",esp,error_code);
}

void do_debug(long esp, long error_code)
{
	die("debug",esp,error_code);
}

void do_overflow(long esp, long error_code)
{
	die("overflow",esp,error_code);
}

void do_bounds(long esp, long error_code)
{
	die("bounds",esp,error_code);
}

void do_invalid_op(long esp, long error_code)
{
	die("invalid operand",esp,error_code);
}

void do_device_not_available(long esp, long error_code)
{
	die("device not available",esp,error_code);
}

void do_coprocessor_segment_overrun(long esp, long error_code)
{
	die("coprocessor segment overrun",esp,error_code);
}

void do_invalid_TSS(long esp,long error_code)
{
	die("invalid TSS",esp,error_code);
}

void do_segment_not_present(long esp,long error_code)
{
	die("segment not present",esp,error_code);
}

void do_stack_segment(long esp,long error_code)
{
	die("stack segment",esp,error_code);
}

void do_coprocessor_error(long esp, long error_code)
{
	if (last_task_used_math != current)
		return;
	die("coprocessor error",esp,error_code);
}

void do_reserved(long esp, long error_code)
{
	die("reserved (15,17-47) error",esp,error_code);
}
/**
 * 异常（陷阱）中断程序初始化子程序。设置中断调用门（中断向量）。set_trap_gate()与
 * set_system_gate()主要区别在于前者设置的特权级为0，后者3.因此端点int3、溢出overflow
 * 和边界出错中断bounds可由任何程序产生。这两个函数均是嵌入式汇编宏 include/asm/system.h
 * */
void trap_init(void)
{
	int i;

	set_trap_gate(0,&divide_error);		// 设置除操作出错的中断向量值，下同
	set_trap_gate(1,&debug);
	set_trap_gate(2,&nmi);
	set_system_gate(3,&int3);	/* int3-5 can be called from all */
	set_system_gate(4,&overflow);
	set_system_gate(5,&bounds);
	set_trap_gate(6,&invalid_op);
	set_trap_gate(7,&device_not_available);
	set_trap_gate(8,&double_fault);
	set_trap_gate(9,&coprocessor_segment_overrun);
	set_trap_gate(10,&invalid_TSS);
	set_trap_gate(11,&segment_not_present);
	set_trap_gate(12,&stack_segment);
	set_trap_gate(13,&general_protection);
	set_trap_gate(14,&page_fault);
	set_trap_gate(15,&reserved);
	set_trap_gate(16,&coprocessor_error);
	for (i=17;i<48;i++)					// 将int17-48的陷阱门先均设置为reserved
		set_trap_gate(i,&reserved);		// 以后各硬件初始化时会重新设置自己的陷阱门
	set_trap_gate(45,&irq13);			// 设置协处理器的陷阱门
	outb_p(inb_p(0x21)&0xfb,0x21);		// 允许主8259A芯片的IRQ2中断请求
	outb(inb_p(0xA1)&0xdf,0xA1);		// 允许从芯片的IRQ13中断请求
	set_trap_gate(39,&parallel_interrupt);	// 设置并行口的陷阱门
}