#
# if you want the ram-disk device, define this to be the
# size in blocks.
#
RAMDISK = #-DRAMDISK=512

AS86	=as86 -0 -a		# 8086汇编器和链接器
LD86	=ld86 -0		# -0生成8086目标程序； -a生成与gas gld部分兼容的代码

AS	=gas				# 当时版本的gnu as和ld
LD	=gld
# 下一行是gld运行时用到的选项。 -s 输出文件中省略所有的符号信息; -x删除所有局部符号;
#							-M 在标准输出设备上打印连接映像link map(由链接程序
#							产生的一种内存地址映像， 其中列出了程序段装入内存的位
#							置信息，包括目标文件及符号信息映射到内存中的位置，公共符号
#							如何放置，连接中包含的所有文件成员及其引用的符号)
LDFLAGS	=-s -x -M
CC	=gcc $(RAMDISK)		# 引用定义的标识符时，加上$()
# gcc选项。 -Wall 打印所有警告信息; -O 代码优化; -fstrength-reduce 优化循环语句
#		   -mstring-insns linus自己增加的选项，用于对字符串指令优化程序，可以去掉
CFLAGS	=-Wall -O -fstrength-reduce -fomit-frame-pointer \
-fcombine-regs -mstring-insns
# gcc预处理程序。 不使用标准目录头文件，使用-I选项制定目录或在当前目录里搜索头文件
CPP	=cpp -nostdinc -Iinclude

#
# ROOT_DEV specifies the default root-device when making the image.
# This can be either FLOPPY, /dev/xxxx or empty, in which case the
# default of /dev/hd6 is used by 'build'.
#
ROOT_DEV=/dev/hd6

# kernel mm fs目录产生的目标代码文件，用ARCHIVES标识符表示
ARCHIVES=kernel/kernel.o mm/mm.o fs/fs.o
# 块和字符设备库文件。 .a表示归档文件，即包含许多可执行二进制代码子程序集合的库文件
# 通常由gnu ar生成
DRIVERS =kernel/blk_drv/blk_drv.a kernel/chr_drv/chr_drv.a
# 数学库运算符文件
MATH	=kernel/math/math.a
# lib目录中文件编译成的通用库文件
LIBS	=lib/lib.a

.c.s:	# make的老式隐式后缀规则，指示以下命令将所有.c文件编译生成.s汇编程序
		# ':'表示下面是该规则的命令
	$(CC) $(CFLAGS) \
	-nostdinc -Iinclude -S -o $*.s $< # 仅使用include/中的头文件， 生成汇编文件
									  # $*.s是自动目标变量 $<表示第一个先决条件（文件.c）
.s.o:
	$(AS) -c -o $*.o $<		# -c表示只编译或汇编但不链接
.c.o:
	$(CC) $(CFLAGS) \
	-nostdinc -Iinclude -c -o $*.o $<

all:	Image				# all表示创建Makefile所知的最顶层目标，Image文件
# 此行表示Image由分号后的四个文件产生
Image: boot/bootsect boot/setup tools/system tools/build
# 以下两行为执行的命令，用工具程序build将bootsect setup system文件以
# $(ROOT_DEV)为根文件系统设备组装成内核映像文件Image， sync迫使缓冲数据立即写盘病更新超级块
	tools/build boot/bootsect boot/setup tools/system $(ROOT_DEV) > Image
	sync

# 目标disk由Image产生。dd为UNIX命令：复制一个文件，根据选项进行转换和格式化。
# bs=表示一次读/写的字节数。if=表示输入的文件 of=表示输出到的文件
# /dev/PS0为设备文件，指第一个软盘驱动器
disk: Image
	dd bs=8192 if=Image of=/dev/PS0

tools/build: tools/build.c		# 生成build
	$(CC) $(CFLAGS) \
	-o tools/build tools/build.c

boot/head.o: boot/head.s		# 使用line48行给出的规则生成head.o目标文件

tools/system:	boot/head.o init/main.o \
		$(ARCHIVES) $(DRIVERS) $(MATH) $(LIBS)
	$(LD) $(LDFLAGS) boot/head.o init/main.o \
	$(ARCHIVES) \
	$(DRIVERS) \
	$(MATH) \
	$(LIBS) \
	-o tools/system > System.map		# >表示gld需要将链接映像重定向保存在System.map文件中

kernel/math/math.a:						# 数学协处理函数文件math.a由下一行命令实现
	(cd kernel/math; make)

kernel/blk_drv/blk_drv.a:
	(cd kernel/blk_drv; make)			# 进入目录，运行make

kernel/chr_drv/chr_drv.a:
	(cd kernel/chr_drv; make)

kernel/kernel.o:
	(cd kernel; make)

mm/mm.o:
	(cd mm; make)

fs/fs.o:
	(cd fs; make)

lib/lib.a:
	(cd lib; make)

boot/setup: boot/setup.s
	$(AS86) -o boot/setup.o boot/setup.s
	$(LD86) -s -o boot/setup boot/setup.o

boot/bootsect:	boot/bootsect.s
	$(AS86) -o boot/bootsect.o boot/bootsect.s
	$(LD86) -s -o boot/bootsect boot/bootsect.o

# 在bootsect.s程序开头添加一行有关system文件长度的信息
# 先生成含有SYSSIZE = system文件实际长度 一行信息的tmp.s文件，再将bootsect.s添加在其后
# 使用ls对system文件进行长列表显示，用grep取得列表行上文件字节数字段信息，并定向保存
# 在tmp.s中。cut用于剪切字符串，tr用于去除行尾的回车符。（实际长度+15）/16用于获得
# 节表示的长度信息。1节=16字节。
tmp.s:	boot/bootsect.s tools/system
	(echo -n "SYSSIZE = (";ls -l tools/system | grep system \
		| cut -c25-31 | tr '\012' ' '; echo "+ 15 ) / 16") > tmp.s
	cat boot/bootsect.s >> tmp.s

clean:
	rm -f Image System.map tmp_make core boot/bootsect boot/setup
	rm -f init/*.o tools/system tools/build boot/*.o
	(cd mm;make clean)
	(cd fs;make clean)
	(cd kernel;make clean)
	(cd lib;make clean)

# 首先执行clean，使用tar cf对linux/目录执行归档程序， compress将其压缩为backup.Z
backup: clean
	(cd .. ; tar cf - linux | compress - > backup.Z)
	sync			# 迫使缓冲块数据立即写盘并更新磁盘超级块

# 各文件依赖关系，以便make确定是否需要重建一个目标对象
dep:
	sed '/\#\#\# Dependencies/q' < Makefile > tmp_make
	(for i in init/*.c;do echo -n "init/";$(CPP) -M $$i;done) >> tmp_make
	cp tmp_make Makefile
	(cd fs; make dep)
	(cd kernel; make dep)
	(cd mm; make dep)

### Dependencies:
init/main.o : init/main.c include/unistd.h include/sys/stat.h \
  include/sys/types.h include/sys/times.h include/sys/utsname.h \
  include/utime.h include/time.h include/linux/tty.h include/termios.h \
  include/linux/sched.h include/linux/head.h include/linux/fs.h \
  include/linux/mm.h include/signal.h include/asm/system.h include/asm/io.h \
  include/stddef.h include/stdarg.h include/fcntl.h 
