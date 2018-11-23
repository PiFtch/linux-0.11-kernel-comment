!
! SYS_SIZE is the number of clicks (16 bytes) to be loaded.
! 0x3000 is 0x30000 bytes = 196kB, more than enough for current
! versions of linux
!
SYSSIZE = 0x3000	! 指system模块的大小， 最大默认值为3000, 确定大小见总Makefile
!
!	bootsect.s		(C) 1991 Linus Torvalds
!
! bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
! iself out of the way to address 0x90000, and jumps there.
!
! It then loads 'setup' directly after itself (0x90200), and the system
! at 0x10000, using BIOS interrupts. 
!
! NOTE! currently system is at most 8*65536 bytes long. This should be no
! problem, even in the future. I want to keep it simple. This 512 kB
! kernel size should be enough, especially as this doesn't contain the
! buffer cache as in minix
!
! The loader has been made as simple as possible, and continuos
! read errors will result in a unbreakable loop. Reboot by hand. It
! loads pretty fast by getting whole sectors at a time whenever possible.

! intel汇编
.globl begtext, begdata, begbss, endtext, enddata, endbss	! 定义了6个全局标识符
.text			! 文本段
begtext:		
.data			! 数据段 
begdata:
.bss			! 未初始化数据段
begbss:
.text			! 文本段

SETUPLEN = 4				! nr of setup-sectors			! setup的扇区数
BOOTSEG  = 0x07c0			! original address of boot-sector	! bootsect的原始地址
INITSEG  = 0x9000			! we move boot here - out of the way	！ 移动bootsect至此
SETUPSEG = 0x9020			! setup starts here						！ setup开始处
SYSSEG   = 0x1000			! system loaded at 0x10000 (65536).		! system加载处
ENDSEG   = SYSSEG + SYSSIZE		! where to stop loading				！ 停止加载的段地址

! ROOT_DEV:	0x000 - same type of floppy as boot.
!		0x301 - first partition on first drive etc
ROOT_DEV = 0x306		! 指定根文件系统设备是第2个硬盘的第1个分区

entry start				! 告知链接程序， 程序从start标号开始执行
start:
	mov	ax,#BOOTSEG		! 将0x07c0开始的bootsect移动至0x9000
	mov	ds,ax
	mov	ax,#INITSEG
	mov	es,ax
	mov	cx,#256			! repeat count 256; 256 words
	sub	si,si			! source ds:si = 0x07c0:0x0000
	sub	di,di			! dest	 es:di = 0x9000:0x0000
	rep
	movw				! move 1 word per time
	jmpi	go,INITSEG	! 间接跳转，INITSEG指出跳转到的段地址，即跳转到INITSEG位置，从go处开始执行
go:	mov	ax,cs			! cs段地址，为INITSEG=0x9000
	mov	ds,ax			! 将ds es置为0x9000, 设置堆栈
	mov	es,ax
! put stack at 0x9ff00.	! 堆栈指针sp指向0x9000:0xff00
	mov	ss,ax			！ 堆栈段首地址0x9000, 偏移地址sp
	mov	sp,#0xFF00		! arbitrary value >>512
! 由于先前移动了代码段的位置，堆栈段也要重新设置，setup大小4个扇区，即4 * 0x200, 再加上堆栈大小，以及0x200
! load the setup-sectors directly after the bootblock.
! Note that 'es' is already set up. es = 0x9000

! BIOS中断int 0x13， ah=0x02-读磁盘扇区到内存，al=需要读出的扇区数量，ch=磁道号低8位，cl=开始扇区(位0-5)，磁道号高2位(6-7),dh=磁头号,dl=驱动器号
! es:bx指向数据缓冲区，若出错则CF标志置位
load_setup:
	mov	dx,#0x0000		! drive 0, head 0
	mov	cx,#0x0002		! sector 2, track 0
	mov	bx,#0x0200		! address = 512, in INITSEG 0x9020
	mov	ax,#0x0200+SETUPLEN	! service 2, nr of sectors
	int	0x13			! read it
	jnc	ok_load_setup		! ok - continue		正常，继续
	mov	dx,#0x0000
	mov	ax,#0x0000		! reset the diskette	复位磁盘
	int	0x13
	j	load_setup

ok_load_setup:

! Get disk drive parameters, specifically nr of sectors/track
! 读取磁盘驱动器参数，特别是每道扇区数量。
! INT 0x13取磁盘驱动器参数 -> ah=0x08;dl=驱动器号（若为硬盘则置位7为1）。
! 返回：ah=0,al=0;bl=驱动器类型（AT/PS2）;
! ch=最大磁道号低8位,cl=每磁道最大扇区数（0-5）最大磁道号高2位（6-7），dh=最大磁头数
! dl=驱动器数量，es:di->软驱磁盘参数表。若出错则CF置位，且ah=状态码

	mov	dl,#0x00
	mov	ax,#0x0800		! AH=8 is get drive parameters
	int	0x13
	mov	ch,#0x00
	seg cs				! 表示下一条语句的操作数在cs段寄存器所指的段中
	mov	sectors,cx		! 保存每磁道扇区数
	mov	ax,#INITSEG
	mov	es,ax			! 13号中断改变了es，将其改回

! Print some inane message	! 显示信息

	mov	ah,#0x03		! read cursor pos
	xor	bh,bh			! 读光标位置
	int	0x10
	
	mov	cx,#24			! 共24个字符
	mov	bx,#0x0007		! page 0, attribute 7 (normal)
	mov	bp,#msg1		! 指向要显示的字符串
	mov	ax,#0x1301		! write string, move cursor
	int	0x10

! ok, we've written the message, now
! we want to load the system (at 0x10000)

	mov	ax,#SYSSEG
	mov	es,ax		! segment of 0x010000	es=system段地址
	call	read_it	! 读磁盘上system模块，es为输入参数
	call	kill_motor	! 关闭电机，获得驱动器状态

! After that we check which root-device to use. If the device is
! defined (!= 0), nothing is done and the given device is used.
! Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
! on the number of sectors that the BIOS reports currently.
! linux中软驱的主设备号是2,次设备号为type×4+nr，nr为0-3对应软驱ABCD，type为软驱类型，2->1.2MB,7->1.44MB
! 7*4+0 = 28,/dev/PS0 (2,28)指1.44MB的A驱动器，其设备号为0x021c;/dev/at0 (2,8)是1.2MB的A驱动器，设备号为0x0208
	seg cs
	mov	ax,root_dev		! 取508 509字节处的根设备号并判断是否已定义
	cmp	ax,#0
	jne	root_defined
	seg cs
	mov	bx,sectors		! 取保存的每磁道扇区数，若为15,是1.2MB驱动器，18则为1.44MB软驱，因为是可引导驱动器，所以肯定是A驱
	mov	ax,#0x0208		! /dev/ps0 - 1.2Mb
	cmp	bx,#15
	je	root_defined
	mov	ax,#0x021c		! /dev/PS0 - 1.44Mb
	cmp	bx,#18
	je	root_defined
undef_root:
	jmp undef_root		! 未指定设备，则死循环
root_defined:
	seg cs
	mov	root_dev,ax		! 将检查过的设备号保存起来

! after that (everyting loaded), we jump to
! the setup-routine loaded directly after
! the bootblock:

	jmpi	0,SETUPSEG	! 跳转

! This routine loads the system at address 0x10000, making sure
! no 64kB boundaries are crossed. We try to load it as fast as
! possible, loading whole tracks whenever we can.
!
! in:	es - starting address segment (normally 0x1000)
!
sread:	.word 1+SETUPLEN	! sectors read of current track
head:	.word 0			! current head
track:	.word 0			! current track

read_it:
	mov ax,es
	test ax,#0x0fff		! 将ax与0fff进行与运算，若为0置ZF=1,不改变ax
die:	jne die			! es must be at 64kB boundary	若ax=1000,ZF=0
	xor bx,bx		! bx is starting address within segment bx为段内偏移
rp_read:		! 判断是否已经读入全部数据，比较当前所读段是否就是系统数据末端所处的段，如果不是跳转至ok1_read，否则退出子程序v返回
	mov ax,es
	cmp ax,#ENDSEG		! have we loaded all yet?
	jb ok1_read			! 无符号小于则跳转，即ax未达到末端
	ret
ok1_read:		! 计算和验证当前磁道需要读取的扇区数，放在ax中。根据当前磁道还未读取的扇区数以及段内数据字节开始偏移位置，计算如果全部读取这些未读扇区
				! 所读总字节数是否会超过64KB段长限制，若超过，则根据此次最多能读入的字节数（64KB-段内偏移位置），反算出此次需要读取的扇区数
	seg cs
	mov ax,sectors
	sub ax,sread	! 减去当前磁道已读扇区数
	mov cx,ax		! cx = ax = 当前磁道未读扇区数
	shl cx,#9		! cx = cx × 512（左移9位）
	add cx,bx		! cx = cx + 段内偏移
	jnc ok2_read	! 未进位则跳转，即未超过64KB
	je ok2_read
	xor ax,ax
	sub ax,bx
	shr ax,#9
ok2_read:			! 
	call read_track
	mov cx,ax		! cx = 该次操作已读取的扇区数
	add ax,sread	! 当前磁道上已经读取的扇区数
	seg cs
	cmp ax,sectors	! 如果当前磁道上还有扇区未读，则跳转至ok3
	jne ok3_read
	mov ax,#1		! 读磁道另一磁头上的数据，如果已经完成，则去读下一磁道
	sub ax,head		! 判断当前磁头号
	jne ok4_read	! 如果0是磁头，再去读1磁头面上的扇区数据
	inc track		! 否则去读下一磁道
ok4_read:
	mov head,ax		! 保存当前磁头号
	xor ax,ax		! 清除当前磁道已读扇区数
ok3_read:
	mov sread,ax	! 保存当前磁道已读扇区数
	shl cx,#9		! 上次已读扇区数×512
	add bx,cx		! 调整当前段内数据开始位置
	jnc rp_read		! 若小于64KB则跳转，否则调整当前段为读下一段数据做准备
	mov ax,es
	add ax,#0x1000	! 将段基址调整为指向下一个64KB内存开始处
	mov es,ax
	xor bx,bx		! 清段内数据开始偏移值
	jmp rp_read		! 跳转，继续读数据
! 读当前磁道上指定开始扇区和需读扇区数的数据到es:bx开始处
read_track:
	push ax
	push bx
	push cx
	push dx
	mov dx,track	! 取当前磁道号
	mov cx,sread	! 取当前磁道上已读扇区数
	inc cx			! cl=开始读扇区
	mov ch,dl		! ch=当前磁道号
	mov dx,head		! 取当前磁头号
	mov dh,dl		! dh=磁头号
	mov dl,#0		! dl=驱动器号，0表示当前驱动器
	and dx,#0x0100	! 磁头号不大于1
	mov ah,#2		! ah=2,读磁盘扇区功能号
	int 0x13
	jc bad_rt		! 若出错，跳转
	pop dx
	pop cx
	pop bx
	pop ax
	ret
bad_rt:	mov ax,#0	! 执行驱动器复位操作（磁盘中断功能号0），再跳到read_track处重试
	mov dx,#0
	int 0x13
	pop dx
	pop cx
	pop bx
	pop ax
	jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
	push dx
	mov dx,#0x3f2	! 软驱控制卡的驱动端口，只写
	mov al,#0		! A驱动器，关闭FDC，禁止DMA和中断请求，关闭电动机
	outb			! 将al中的内容输出到dx指定端口去
	pop dx
	ret

sectors:
	.word 0			! 存放当前启动软盘每磁道的扇区数

msg1:
	.byte 13,10		! 回车换行ASCII码
	.ascii "Loading system ..."
	.byte 13,10,13,10	! 共24个ASCII码字符

.org 508	! 表示以下语句从地址508开始，所以root_dev在启动扇区第508开始的2B中
root_dev:
	.word ROOT_DEV	! 存放根文件系统所在的设备号（init/main.c用到）
boot_flag:
	.word 0xAA55	! 硬盘有效标识

.text
endtext:
.data
enddata:
.bss
endbss:
