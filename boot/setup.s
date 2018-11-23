!
!	setup.s		(C) 1991 Linus Torvalds
!
! setup.s is responsible for getting the system data from the BIOS,
! and putting them into the appropriate places in system memory.
! both setup.s and system has been loaded by the bootblock.
!
! This code asks the bios for memory/disk/other parameters, and
! puts them in a "safe" place: 0x90000-0x901FF, ie where the
! boot-block used to be. It is then up to the protected mode
! system to read them from there before the area is overwritten
! for buffer-blocks.
!

! NOTE! These had better be the same as in bootsect.s!

INITSEG  = 0x9000	! we move boot here - out of the way
SYSSEG   = 0x1000	! system loaded at 0x10000 (65536).
SETUPSEG = 0x9020	! this is the current segment

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

entry start
start:

! ok, the read went well so we get current cursor position and save it for
! posterity.

	mov	ax,#INITSEG	! this is done in bootsect already, but...
	mov	ds,ax
	mov	ah,#0x03	! read cursor pos BIOS中断的读光标功能号0x10,ah=0x03. input:bh=page number, return:ch=扫描开始线，cl=扫描结束线，dh=行号(0x00为顶端)，dl=列号(0x00是左边)
	xor	bh,bh
	int	0x10		! save it in known place, con_init fetches
	mov	[0],dx		! it from 0x90000.		! 光标信息存放在0x90000

! Get memory size (extended mem, kB)
! BIOS中断0x15，功能号ah=0x88.返回ax=从0x100000(1M)处开始的扩展内存大小
	mov	ah,#0x88
	int	0x15
	mov	[2],ax		! 扩展内存数值存放在0x90002(word)

! Get video-card data:
! BIOS中断0x10，功能号ah=0x0f.返回ah=字符列数,al=显示模式,bh=当前显示页
	mov	ah,#0x0f
	int	0x10
	mov	[4],bx		! bh = display page
	mov	[6],ax		! al = video mode, ah = window width

! check for EGA/VGA and some config parameters
! 功能号ah=0x12,bl=0x10, 返回bh=显示状态(0x00彩色模式,IO端口=0x3dX;0x01单色模式，IO端口0x3bX)，bl=安装的显示内存(0x00-64k,0x01-128k,0x02-192k,0x03-256k),cx=显示卡特性参数
	mov	ah,#0x12
	mov	bl,#0x10
	int	0x10
	mov	[8],ax
	mov	[10],bx
	mov	[12],cx

! Get hd0 data
! 取第一个硬盘的信息，第一个硬盘参数表首地址是中断向量0x41的向量值，第二个硬盘参数表紧接第一个表之后，中断向量0x46的向量值也指向这第二个硬盘的参数表首地址，表长16字节
	mov	ax,#0x0000
	mov	ds,ax			 ! 初始化时中断向量表从0x00000开始
	lds	si,[4*0x41]      ! 取中断向量0x41的值，即hd0的参数表地址ds:si
	mov	ax,#INITSEG		 ! 0x9000
	mov	es,ax
	mov	di,#0x0080		 ! 参数表送至es:di,共16字节
	mov	cx,#0x10
	rep
	movsb

! Get hd1 data
! 同上
	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x46]
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	rep
	movsb

! Check that there IS a hd1 :-)
! 检查是否存在第二个硬盘，如果不存在则第二个表清零
! BIOS中断0x13的取盘类型功能，功能号ah=0x15, 输入dl=驱动器号(0x8X是硬盘,0x80指第一个硬盘，0x81指第二个硬盘).
! 输出ah=类型码，00-没有这个盘，CF置位， 01-软驱，没有change-line支持，02-软驱或其他可移动设备，有change-line支持， 03-硬盘
	mov	ax,#0x01500
	mov	dl,#0x81
	int	0x13
	jc	no_disk1
	cmp	ah,#3
	je	is_disk1
no_disk1:
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	mov	ax,#0x00
	rep
	stosb		! 使用al的值填充es:di，即将第二个参数表清零
is_disk1:

! now we want to move to protected mode ...

	cli			! no interrupts allowed !	! 关中断

! first we move the system to it's rightful place
! bootsect将system读入0x10000开始的位置，由于当时假设system长度不会超过0x80000(512KB),即末端不会超过0x90000，所以bootsect将自己移动至0x90000,
! 并将setup加载于其后。以下程序将system移动至0x00000，即把从0x10000到0x8ffff的内存数据块(512KB)整块向内存低端移动了0x10000(64KB)位置。
	mov	ax,#0x0000
	cld			! 'direction'=0, movs moves forward
do_move:
	mov	es,ax		! destination segment 目的es=0x0000
	add	ax,#0x1000
	cmp	ax,#0x9000
	jz	end_move
	mov	ds,ax		! source segment
	sub	di,di		! di=0x0000
	sub	si,si		! ds:si 0x1000:0x0000
	mov 	cx,#0x8000	! move 0x8000words(64KB)
	rep
	movsw
	jmp	do_move

! then we load the segment descriptors

end_move:
	mov	ax,#SETUPSEG	! right, forgot this at first. didn't work :-) 0x9020
	mov	ds,ax			! 9020即为setup段位置
	lidt	idt_48		! load idt with 0,0	加载IDTR，描述于文件最后
	lgdt	gdt_48		! load gdt with whatever appropriate

! that was painless, now we enable A20
! 开启A20地址线
	call	empty_8042 	! i8042键盘控制器，等待输入缓冲器空，只有为空时才可对其写命令
	mov	al,#0xD1		! command write 0xD1命令码，表示要写数据到8042 P2口，P2口位1用于A20线选通，数据写到0x60口
	out	#0x64,al
	call	empty_8042	! 等待输入缓冲器空，看命令是否被接受
	mov	al,#0xDF		! A20 on	选通A20地址线的参数
	out	#0x60,al
	call	empty_8042	! 输入缓冲器为空，表示A20线已经选通

! well, that went ok, I hope. Now we have to reprogram the interrupts :-(
! we put them right after the intel-reserved hardware interrupts, at
! int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
! messed this up with the original PC, and they haven't been able to
! rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
! which is used for the internal hardware interrupts as well. We just
! have to reprogram the 8259's, and it isn't fun.

	mov	al,#0x11		! initialization sequence
	! 0x11表示初始化命令开始，是ICW1命令字，表示边沿触发，多面8259级联，最后要发ICW4命令字
	out	#0x20,al		! send it to 8259A-1 发送到8259A主芯片
	.word	0x00eb,0x00eb		! jmp $+2, jmp $+2
	out	#0xA0,al		! and to 8259A-2 再发送到从芯片
	.word	0x00eb,0x00eb
	mov	al,#0x20		! start of hardware int's (0x20)
	out	#0x21,al		! 送主芯片ICW2命令字，起始中断号，要送奇地址
	.word	0x00eb,0x00eb
	mov	al,#0x28		! start of hardware int's 2 (0x28)
	out	#0xA1,al		! 送从芯片ICW2命令字，从芯片的起始中断号
	.word	0x00eb,0x00eb
	mov	al,#0x04		! 8259-1 is master
	out	#0x21,al		! 送主芯片ICW3命令字，主芯片IR2连从芯片INT
	.word	0x00eb,0x00eb
	mov	al,#0x02		! 8259-2 is slave
	out	#0xA1,al		! 送从芯片ICW3命令字，表示从芯片INT连主芯片IR2
	.word	0x00eb,0x00eb
	mov	al,#0x01		! 8086 mode for both
	out	#0x21,al		! 送主芯片ICW4命令字，8086模式：普通EOI方式
	.word	0x00eb,0x00eb ! 需发送指令来复位，初始化结束，芯片就绪
	out	#0xA1,al		! 送从芯片ICW4命令字，内容同上
	.word	0x00eb,0x00eb
	mov	al,#0xFF		! mask off all interrupts for now
	out	#0x21,al		! 屏蔽主芯片所有中断请求
	.word	0x00eb,0x00eb 
	out	#0xA1,al		! 屏蔽从芯片所有中断

! well, that certainly wasn't fun :-(. Hopefully it works, and we don't
! need no steenking BIOS anyway (except for the initial loading :-).
! The BIOS-routine wants lots of unnecessary data, and it's less
! "interesting" anyway. This is how REAL programmers do it.
!
! Well, now's the time to actually move into protected mode. To make
! things as simple as possible, we do no register set-up or anything,
! we let the gnu-compiled 32-bit programs do that. We just jump to
! absolute address 0x00000, in 32-bit protected mode.

	mov	ax,#0x0001	! protected mode (PE) bit
	lmsw	ax		! This is it!
	jmpi	0,8		! jmp offset 0 of segment 8 (cs)

! This routine checks that the keyboard command queue is empty
! No timeout is used - if this hangs there is something wrong with
! the machine, and we probably couldn't proceed anyway. 只有输入缓冲为空（状态寄存器位2=0）时才可以对其写命令
empty_8042:
	.word	0x00eb,0x00eb	! 两个跳转指令机器码，跳转到下一句，作为延时空操作
	in	al,#0x64	! 8042 status port	读8042状态寄存器
	test	al,#2		! is input buffer full?	测试位2，输入缓冲为满？
	jnz	empty_8042	! yes - loop
	ret
! 全局描述符表开始处，，每项8B，第一项无用但必须存在，第二项为系统代码段描述符
! 第三项为系统数据段描述符
gdt:
	.word	0,0,0,0		! dummy
! 在GCT表中便宜量为0x08，当加载代码段寄存器（段选择符）时，使用此偏移值
! 保护模式下，段寄存器中保存的不是段地址，而是段选择子，真正的段地址位于段寄存器的描述符高速缓存中
	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9A00		! code read/exec
	.word	0x00C0		! granularity=4096, 386
! 偏移0x10,加载数据段寄存器时使用此值
	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9200		! data read/write
	.word	0x00C0		! granularity=4096, 386

idt_48:	! lidt操作数，共6字节，前2字节为idt表限长，后4字节是idt表所处的基地址
	.word	0			! idt limit=0
	.word	0,0			! idt base=0L

gdt_48: ! lgdt操作数，GDT表长2KB，每8B组成一个段描述符项，即表中共可有256项
	.word	0x800		! gdt limit=2048, 256 GDT entries
	.word	512+gdt,0x9	! gdt base = 0X9xxxx
	
.text
endtext:
.data
enddata:
.bss
endbss:
