/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h> 
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];	// 静态字符串数组，用作内核显示信息的缓存
/* block devices are those that can be read by blocks even though some programs may want to read char by char. These include tapes and hard drives.
Charactor devices are those only read char by char such as keyboards and serial ports.*/
extern int vsprintf();		// 送格式化输出到一字符串中，kernel/vsprintf.c
extern void init(void);
extern void blk_dev_init(void);	// 块设备初始化子程序 blk_drv/ll_rw_blk.c
extern void chr_dev_init(void);	// 字符设备初始化 chr_drv/tty_io.c
extern void hd_init(void);		// 硬盘初始化 blk_drv/hd.c
extern void floppy_init(void);	// 软驱初始化 blk_drv/floppy.c
extern void mem_init(long start, long end);	// 内存管理初始化 mm/memory.c
extern long rd_init(long mem_start, int length);	// 虚拟盘初始化 blk_drv/ramdisk.c
extern long kernel_mktime(struct tm * tm);	// 建立内核时间（秒）
extern long startup_time;	// 内核启动时间（开机时间）（秒）

/*
 * This is set up by the setup-routine at boot-time -- setup.s
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)	// 1MB以后的扩展内存大小（KB）
#define DRIVE_INFO (*(struct drive_info *)0x90080)	// 硬盘参数表基址
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)	// 根文件系统所在设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \ // 读取CMOS实时时钟信息
outb_p(0x80|addr,0x70); \	 // 0x70是写端口号，0x80|addr是要读取的CMOS内存地址
inb_p(0x71); \				 // 0x71是读端口号
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)	// BCD to 数字

static void time_init(void)	// 读取CMOS时钟，并设置开机时间->startup_time
{
	struct tm time;
// 以下循环操作用于控制时间误差在1秒之内
	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;			// tm_mon中月份范围是0-11
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;		// 机器具有的内存（字节数）
static long buffer_memory_end = 0;	// 高速缓冲区末端地址
static long main_memory_start = 0;	// 主内存（将用于分页）开始的位置

struct drive_info { char dummy[32]; } drive_info;	// 用于存放硬盘参数表信息

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */	// head.s
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
/* 保存：根设备号->ROOT_DEV；告诉缓存末端地址->buffer_memory_end;
		机器内存数->memory_end; 主内存开始地址-?main_memory_start */
 	ROOT_DEV = ORIG_ROOT_DEV;	// ROOT_DEV fs/super.c
 	drive_info = DRIVE_INFO;
	memory_end = (1<<20) + (EXT_MEM_K<<10); // 内存大小=1MB+扩展内存KB*1024
	memory_end &= 0xfffff000;	// 忽略不到4KB的内存数
	if (memory_end > 16*1024*1024)	// 超过16MB按16MB计
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 	// 超过12MB，缓冲区末端=4MB
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)	// 超过6MB，设置2MB
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;	// 否则设置1MB
	main_memory_start = buffer_memory_end;	// 主存起始位置=缓冲区末端
#ifdef RAMDISK	// 如果定义了虚拟盘，则初始化虚拟盘，此时主内存将减少
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
// 内核进行所有初始化工作
	mem_init(main_memory_start,memory_end);
	trap_init();	// 中断向量初始化 kernel/traps.c
	blk_dev_init();	// kernel/blk_drv/ll_rw_block.c
	chr_dev_init();
	tty_init();
	time_init();	// 设置开机启动时间->startup_time
	sched_init();	// 调度程序初始化
	buffer_init(buffer_memory_end);
	hd_init();
	floppy_init();
	sti();			// 所有初始化工作完成，开启中断
	// 以下过程通过在堆栈中设置的参数，利用中断返回指令启动第一个任务task0
	move_to_user_mode();	// 移到用户模式下运行
	if (!fork()) {		/* we count on this going ok */
		init();			// 在新建的子进程（任务1）中执行
	}					// 下面的代码开始以任务0的身份运行
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}
/**
 * 以下函数产生格式化信息并输出到标准输出设备stdout(1),此处是指在屏幕上显示。参数fmt制定输出格式。该子程序正好是vsprintf如何使用的例子。
 * 使用vsprintf将格式化字符串放入printbuf缓冲区，再用write()将缓冲区的内容输出到标准输出设备，参见kernel/vspprintf.c
 * */
static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };	// 调用执行程序时参数的字符串数组
static char * envp_rc[] = { "HOME=/", NULL };	// 调用执行程序时的环境字符串数组

static char * argv[] = { "-/bin/sh",NULL };		// 同上
static char * envp[] = { "HOME=/usr/root", NULL };
/**
 * init()函数运行在任务0创建的子进程（任务1）中。它首先对第一个要执行的程序(shell)的环境进行初始化，然后加载该程序并执行之。
 * */
void init(void)
{
	int pid,i;
// 读取硬盘参数包括分区表信息并建立虚拟盘和安装根文件系统设备。该函数是在25行上的宏定义的，对应函数是sys_setup(),在kernel/blk_drv/hd.c
	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0);	// 用读写访问方式打开设备/dev/tty0,此处对应终端控制台。返回的句柄号0--stdin标准输入设备
	(void) dup(0);		// 复制句柄，产生句柄1号-stdout标准输出设备
	(void) dup(0);		// 复制句柄，产生句柄2号-stderr标准出错输出设备
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);		// 打印缓冲区块数和字节总数，每块1024字节
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);	// 空闲内存字节数
	/**
	 * 下面fork用于创建一个子进程，对于被创建的子进程，fork返回0，父进程返回子进程的进程号，以下是子进程执行的内容。
	 * 该子进程关闭了句柄0(stdin)，以只读方式打开/etc/rc文件，并执行/bin/sh程序，所带参数和环境变量分别由argv_rc和envp_rc数组给出。
	 * */
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	/**
	 * 父进程
	 * wait等待子进程停止或种植，其返回值应是子进程进程号。
	 * 作用是父进程等待子进程的结束，&i存放返回状态信息的位置。如果wait返回值不等于子进程号，则继续等待。
	 * */
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	/**
	 * 创建的子进程执行已停止或终止，下面循环中首先再创建一个子进程，如果出错，则显示初始化程序创建子进程失败的信息并继续执行。
	 * 对于所创建的子进程，关闭所有以前遗留的句柄(stdin,stdout,stderr)，新创建一个会话并设置进程组号，然后荣新打开/dev/tty0作为stdin，
	 * 并复制成stdout和stderr。再次执行/bin/sh。但这次执行所选用的参数和环境数组另选了一套，然后父进程再次运行wait等待，如果子进程又停止
	 * 执行，则在标准输出上显示出错信息子进程pid停止了运行，返回码是i，然后继续重试下去。
	 * */
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
