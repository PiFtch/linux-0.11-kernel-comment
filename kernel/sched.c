/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))
/**
 * 显示任务号nr的进程号，进程状态和内核堆栈空闲字节数
 * */
void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])	// 检测指定任务数据结构之后等于0的字节数
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}
// 显示所有任务的任务号，进程号，进程状态和内核堆栈空闲字节数
void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])		// task[i]非空
			show_task(i,task[i]);
}
// 设置定时芯片8253初值
#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);	// 时钟中断处理程序 system_call.s
extern int system_call(void);		// 系统调用中断处理程序

union task_union {		// 定义任务联合（任务结构成员和stack字符数组程序成员
	struct task_struct task;	// 因为任务数据结构与其堆栈放在同一内存页中，所以
	char stack[PAGE_SIZE];		// 从SS可以获得其数据段选择符
};
// 定义初始任务的数据
static union task_union init_task = {INIT_TASK,};
// volatile要求gcc不要对该变量优化处理，也不要挪动位置，因为别的程序回来修改它的值
long volatile jiffies=0;	// 从开机开始算起的滴答数（10ms一次）
long startup_time=0;
struct task_struct *current = &(init_task.task);	// 当前任务指针
struct task_struct *last_task_used_math = NULL;		// 使用过协处理器任务的指针

struct task_struct * task[NR_TASKS] = {&(init_task.task), }; //任务指针数组

long user_stack [ PAGE_SIZE>>2 ] ;	// 定义堆栈任务0的用户态，4K
// 该结构用于设置堆栈ss:esp（数据段选择符），见head.s
struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
/**
 * 将当前协处理器内容保存到老协处理器状态数组中，并将当前任务的协处理器内容加载进协处理器
 * 当任务被调度交换过以后，该函数用以保存原任务的协处理器状态并恢复调度进来的当前任务的协处理器执行状态
 * */
void math_state_restore()
{
	if (last_task_used_math == current)		// 如果任务没变则返回（上一个任务是当前任务）
		return;								// 上个任务是刚被交换出去的任务
	__asm__("fwait");						// 在发送协处理器命令之前要先发WAIT指令
	if (last_task_used_math) {				// 如果上个任务使用了协处理器，则保存其状态
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {				// 如果当前任务用过协处理器，则恢复其状态
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {								// 否则说明是第一次使用
		__asm__("fninit"::);				// 向协处理器发初始化命令
		current->used_math=1;				// 设置使用了协处理器标志
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;
// 检测alarm（进程的报警定时值），唤醒任何已得到信号的可中断任务
/* check alarm, wake up any interruptible tasks that have got a signal */
// 从任务数组中最后一个任务开始检测alarm
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {		// 如果设置过任务的alarm并且任务的alarm时间已经过期，则在信号位图中设置SIGALRM信号
					(*p)->signal |= (1<<(SIGALRM-1));		// 然后清alarm。jiffies是系统从开机开始算起的滴答数
					(*p)->alarm = 0;
				}
			/**
			 * 如果信号位图中除被阻塞的信号外还有其他信号，并且任务处于可中断状态，则置任务为就绪状态。
			 * ~(_BLOCKABLE & (*p)->blocked)用于忽略被阻塞的信号，但SIGKILL和SIGSTOP不可被阻塞
			 * */
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;		// 跳过不含任务的槽
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;	// 使next指向counter最大的就绪状态（counter是任务运行时间的递减计数），即选择运行时间短的任务
		}
		if (c) break;	// 如果找到了这样的任务，则退出外层循环
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)		// 否则根据每个任务的优先值，更新counter后重新比较
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	switch_to(next);	// 切换到任务号为next的任务，并运行之，若无则运行任务0
}
/**
 * pause系统调用，转换当前任务的状态为可中断的等待状态，并重新调度。
 * 该系统调用将导致进程进入睡眠状态，直到收到一个信号。该信号用于终止进程或者使进程调用一个信号捕获函数。
 * 只有当捕获了一个信号，并且信号捕获处理函数返回，pause才会返回，此时pause返回值应是-1，且errno被置为EINTR，此处还没有完全实现。
 * */
int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}
/**
 * 把任务置位不可中断的等待状态，并让睡眠队列头的指针指向当前任务。
 * 只有明确地唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。参数*p是放置等待任务的队列头指针。
 * */
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)		// 指针无效，退出
		return;
	if (current == &(init_task.task))	// 任务0不可睡眠，此时死机
		panic("task[0] trying to sleep");
	tmp = *p;		// 让tmp指向已经在等待队列上的任务（如果有的话）
	*p = current;	// 将睡眠队列头的等待指针指向当前任务
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();		// 重新调度
	if (tmp)		// 若还存在等待的任务，则也将其置为就绪状态（唤醒）
		tmp->state=0;
}
// 将当前任务置为可中断的等待状态，并放入*p指定的等待队列中
void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	/**
	 * 如果等待队列中还有等待任务，并且队列头指针所指向的任务不是当前任务时，则将该等待任务置为就绪状态，
	 * 并重新执行调度程序。当指针*p所指向的不是当前任务时，表示在当前任务被放入队列后，又有新的任务被插入等待队列中，因此，就应该
	 * 将其他等待任务也置为可运行态。
	 * */
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	// 应该是*p=tmp，让队列头指针指向其余等待任务，否则在当前任务之前插入等待队列的任务均被抹掉了，也需要删掉wake_up中的这一句。
	*p=NULL;
	if (tmp)
		tmp->state=0;
}
// 唤醒指定任务*p
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};	// 等待电动机加速进程的指针数组
static int  mon_timer[4]={0,0,0,0};		// 存放软驱启动加速所需时间数（滴答数）
static int moff_timer[4]={0,0,0,0};		// 存放软驱电动机停转之前需维持L时间，默认为10000滴答
unsigned char current_DOR = 0x0C;		// 数字输出寄存器（初值：允许DMA和请求中断、启动FDC）
// 指定软盘到正常运转状态所需延迟滴答数（时间）。 nr--软驱号（0-3），返回值为滴答数。
int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;		// 当前选中的软盘号 blk_drv/floppy.c
	unsigned char mask = 0x10 << nr;	// 所选软驱对应数字输出寄存器中启动马达比特位

	if (nr>3)							// 最多4个软驱
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {		// 若非当前软驱，则先复位其他软驱选择位，然后置对应软驱选择位
		mask &= 0xFC;
		mask |= nr;
	}
	/**
	 * 如果数字输出寄存器的值与要求的不同，则向FDC数字输出端口输出新值（mask）。
	 * 并且如果要求启动的电动机还没有启动，则置相应软驱的电动机启动定时器值（HZ/2=0.5s或50个滴答）。
	 * 此后更新当前数字输出寄存器值current_DOR。
	 * */
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}
// 等待软驱电动机启动所需时间
void floppy_on(unsigned int nr)
{
	cli();			// 关中断
	while (ticks_to_floppy_on(nr))	// 如果电动机启动定时还没到，就一直把当前进程置为不可中断睡眠状态并放入等待电动机运行的队列中。
		sleep_on(nr+wait_motor);
	sti();			// 开中断
}
// 置关闭相应软驱电动机停转定时器（3秒）。
void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}
// 软盘定时处理子程序。更新电动机启动定时值和电动机关闭停转计时值。
// 该子程序是在时钟定时中断中被调用，因此每一个滴答被调用一次，更新电动机开启或停转定时器的值。
// 如果某一个电动机停转定时到，则将数字输出寄存器电动机启动位复位。
void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))		// 如果不是DOR指定的电动机则跳过
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);	// 如果电动机启动定时到则唤醒进程
		} else if (!moff_timer[i]) {	// 如果电动机停转定时到则复位相应电动机启动位，并更新数字输出寄存器
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;			// 电动机停转计时递减
	}
}

#define TIME_REQUESTS 64				// 最多可有64个定时器链表
// 定时器链表结构和定时数组
static struct timer_list {
	long jiffies;						// 定时滴答数
	void (*fn)();						// 定时处理程序
	struct timer_list * next;			// 下一个定时器
} timer_list[TIME_REQUESTS], * next_timer = NULL;
// 添加定时器。输入参数为指定的定时值和相应的处理程序指针
void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)	// 如果定时处理程序指针为空，则退出
		return;
	cli();
	if (jiffies <= 0)	// 若定时值<=0，则立即调用处理程序，且该定时器不加入链表中
		(fn)();
	else {		// 从数组中找一个空位
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");	// 已经用完定时器数组
		p->fn = fn;			// 想定时器结构填入信息，链入链表
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		/**
		 * 链表项按定时值从小到大排序，在排序时减去排在前面需要的滴答数，这样在处理定时器时只要
		 * 查看链表头的第一项的定时是否到期即可。
		 * */
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}
/**
 * 时钟中断C函数处理程序，在system_call.s中_timer_interrupt被调用。
 * 参数cpl是当前特权级0或3,0是内核代码。
 * 对于一个进程，由于执行时间片用完时，则进行任务切换，并执行一个计时更新工作。
 * */
void do_timer(long cpl)
{
	extern int beepcount;	// 扬声器发生时间滴答数 kernel.chr_drv_console.c
	extern void sysbeepstop(void);		// 关闭扬声器

	if (beepcount)			// 如果发声计数次数到，则关闭发声。
		if (!--beepcount)	// 向0x61口发送命令，复位位0和1
			sysbeepstop();	// 位0控制8253计数器2的工作，位1控制扬声器。
/**
 * 如果当前特权级为最0，将内核运行时间stime递增，否则增加utime
 * */
	if (cpl)
		current->utime++;
	else
		current->stime++;
// 如果有用户定时器存在，则将链表第1个定时器的值减1.如果已等于0，则调用相应的处理程序，
// 并将该处理程序指针置为空，去掉该项定时器
	if (next_timer) {				// next_timer是定时器链表的头指针
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);		// 这里插入了一个函数指针定义
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	// 如果当前软盘控制器FDC的数字输出寄存器中电动机启动位有置位的，则执行软盘定时程序。
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;		// 如果进程运行时间还没完，则退出
	current->counter=0;
	if (!cpl) return;	// 对于内核程序，不依赖counter值进行调度。
	schedule();
}
// 系统调用功能-设置报警定时时间值，若已设置过alarm值，则返回旧值，否则返回0.
int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}
// 取当前进程号pid
int sys_getpid(void)
{
	return current->pid;
}
// 取父进程号
int sys_getppid(void)
{
	return current->father;
}
// 取用户号
int sys_getuid(void)
{
	return current->uid;
}
// 取有效用户号
int sys_geteuid(void)
{
	return current->euid;
}
// 取组号
int sys_getgid(void)
{
	return current->gid;
}
// 取有效组号
int sys_getegid(void)
{
	return current->egid;
}
// 系统调用功能--降低对CPU的使用优先权。应该限制increment>0，否则可使优先权增大
int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}
// 调度程序的初始化子程序
void sched_init(void)
{
	int i;
	struct desc_struct * p;	// 描述符表指针结构

	if (sizeof(struct sigaction) != 16)		// sigaction存放有关信号状态的结构
		panic("Struct sigaction MUST be 16 bytes");
	// 设置初始任务0的任务状态段描述符和局部数据表描述符 include/asm/system.h
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	// 清任务数组和描述符表项（注意i=1开始，所以初始任务的描述符还在）。
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
// 清楚标识寄存器中的位NT
// NT用于控制程序的递归调用(Nested Task)。当NT置位时，那么当前中断任务执行iret指令时就会引起任务切换。
// NT指出TSS中的back_link字段是否有效。
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");	// 复位NT标志
	ltr(0);						// 将任务0的TSS加载到任务寄存器tr
	lldt(0);					// 将局部描述符表加载到局部描述符表寄存器
	// 是将GDT中相应的LDT描述符的选择符加载到ldtr，只明确加载这一次，以后新任务LDT的加载，是CPU根据TSS中的LDT项自动加载。
	// 初始化8253定时器
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */	// 定时值低字节
	outb(LATCH >> 8 , 0x40);	/* MSB */		// 高字节
	set_intr_gate(0x20,&timer_interrupt);		// 设置时钟中断门
	outb(inb_p(0x21)&~0x01,0x21);				// 修改中断控制器屏蔽码，允许时钟中断
	set_system_gate(0x80,&system_call);			// 设置系统调用中断门
}
