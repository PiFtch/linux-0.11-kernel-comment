/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}
// 取指定设备超级块
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)	// 没有指定设备
		return NULL;
	s = 0+super_block;	// s指向超级块数组开始处，搜索整个超级块数组，寻找指定设备的超级块
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}
// 从设备上读取超级块到缓冲区中
// 如果该设备的超级块已经在高速缓冲中并且有效，则直接返回该超级块的指针
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;
// 如果没有指明设备，则返回空指针
	if (!dev)
		return NULL;
// 首先检查该设备是否可更换过盘片（是否是软盘设备），如果更换过盘，则高速缓冲区有关该设备的所有缓冲块均失效，需要进行失效处理（释放原来加载的文件系统）
	check_disk_change(dev);
// 如果该设备的超级块已经在高速缓冲中，则直接返回该超级块的指针
	if (s = get_super(dev))
		return s;
// 否则，首先在超级块数组中找出一个空项（s_dev=0)，如果数组已经占满则返回空指针
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
// 找到超级块空项后，就将该超级块用于指定设备，对该超级块的内存项进行部分初始化
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
// 然后锁定该超级块，并从设备上读取超级块信息到bh指向的缓冲区中。如果读超级块操作失败，则释放上面选定的超级块数组数组中的想，并解锁该项，返回空指针退出
	lock_super(s);
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
// 将设备上读取的超级块信息复制到超级块数组相应项结构中，并释放存放读取信息的高速缓冲块
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
// 如果读取的超级块的文件系统魔数字段内容不对，说明设备上不是正确的文件系统，因此同上面一样，释放上面选定的超级块数组中的项，并解锁该项，返回空指针退出。
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
// 读取设备上i节点位图和逻辑块位图数据，首先初始化内存超级块结构中的位图空间
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
// 然后从设备上读取i节点位图和逻辑块位图信息，并存放在超级块对应字段中
	block=2;
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
// 如果独处的位图逻辑块数不等于应该占有的逻辑块数，说明文件系统位图信息有问题，超级块初始化失败，因此只能释放前面申请的所有资源，返回空指针并退出
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
// 释放i节点位图和逻辑块位图占用的高速缓冲区
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
// 释放上面选定的超级块数组中的项，并解锁该超级块项，返回空指针退出
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
// 否则一切成功，对于申请空闲i节点的函数来说，如果设备上所有的i节点已经全被使用，则查找函数返回0,因此0号节点不可用，所以将位图中最低位设置为1,防止文件系统
// 分配0号i节点， 同理，逻辑块位图最低位也应设置为1
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);		// 解锁该超级块，并返回超级块指针
	return s;
}
// 卸载文件系统，参数是设备文件名
int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}
// 安装根文件系统
// 在系统开机初始化设置时调用（sys_setup()）， kernel/blk_drv/hd.c
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;
	// i节点结构必须是32字节
	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	// 初始化文件表数组(64项，系统同时只能打开64个文件)，置引用计数为0
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
	// 如果根文件系统所在设备是软盘的话，就提示，并等待按键
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	// 初始化超级块数组
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	// 如果读根设备上超级块失败，则panic
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	// 从设备上读取文件系统的根i节点（1），如果失败则panic
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	// 该i节点引用数递增三次，因为下面也引用了该节点
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	// 置该超级块的被安装文件系统i节点和被安装到的i节点为该i节点
	p->s_isup = p->s_imount = mi;
	// 设置当前进程的当前工作目录和根目录i节点，此时当前进程是1号进程
	current->pwd = mi;
	current->root = mi;
	// 统计该设备上的空闲块数，首先令i等于超级块中表明的设备逻辑块总数
	free=0;
	i=p->s_nzones;
	// 再根据逻辑块位图中相应比特位的占用情况统计出空闲块数
	// set_bit只测试比特位，不设置比特位，i&8191用于去的i节点号在当前块中的偏移值，i>>13是将i除以8192,即除一个磁盘块包含的比特位数
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	// 显示设备上空闲逻辑块数/逻辑块总数
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	// 统计设备上空闲i节点数，先令i等于超级块中表明的设备上i节点数+1（将0节点统计进去）
	free=0;
	i=p->s_ninodes+1;
	// 然后根据i节点位图中相应比特位的占用情况计算出空闲i节点数
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	// 显示设备上可用的空闲i节点数/i节点总数
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
