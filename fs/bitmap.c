/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>
// 将指定地址处的一块内存清零
// in: eax=0, ecx=blocksize/4, edi=addr
#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")
// 置位指定地址处开始的第nr个位偏移处的比特位，返回原比特位（0或1）
// in: %0--eax(返回值) %1--eax(0), %2--nr,位偏移值，%3--addr
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})
// 复位指定地址开始的第nr位便宜处的比特位，返回原比特位的反码（1或0）
// %0--eax（返回值），%1--eax（0）， %2--nr，位偏移值，%3--addr
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})
// 从addr开始寻找第一个0比特位
// %0--ecx返回值，%1--ecx（0），%2--esi（addr）
// 在addr指定地址开始的位图中寻找第一个为0的比特位，并将其距离addr的比特位偏移值返回
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \		// 清方向位
	"1:\tlodsl\n\t" \	// （%esi）-> eax
	"notl %%eax\n\t" \	// eax取反
	"bsfl %%eax,%%edx\n\t" \	// 从位0扫描eax中是1的第一个位，其偏移->edx
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})
// 释放dev上数据区中的逻辑块block
// 复位指定逻辑块的位图比特位
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;
// 取指定设备的超级快
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
// 若逻辑块第一个逻辑块号或大于设备上总逻辑块数，则出错
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
// 从hash表中寻找该块数据，若找到了判断其有效性，并清已修改和更新标志，释放该数据块
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
// 计算block在数据区开始算起的数据逻辑块号（从1开始计数），然后对逻辑块位图复位，若对应位原来就是0,则出错
	block -= sb->s_firstdatazone - 1 ;		// block = block - (-1) 从1开始的块号
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {	// 若原来就为0
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}
// 向dev申请一个逻辑块，返回块号，置位指定逻辑块的位图比特位
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))	// 设备不存在
		panic("trying to get new block from nonexistant device");
	j = 8192;	// 扫描逻辑块位图，寻找第一个0位，寻找空闲逻辑块，获取该块块号
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	// 如果全部扫描完还没找到，或是位图所在的缓冲块无效，则退出
	if (i>=8 || !bh || j>=8192)
		return 0;
	// 设置新逻辑块对应逻辑块位图中的比特位，若之前已置位，则出错
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	// 置对应缓冲块的标志，若新逻辑块大于该设备上的总逻辑块数，则说明指定逻辑块在设备上不存在，退出
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	// 读取设备上的该新逻辑块数据（验证），如果失败则死机
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	// 将该新逻辑块清零，并置位标志，释放对应缓冲区，返回逻辑块号
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}
// 释放指定的i节点，复位对应i节点位图比特位
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) { // 该节点无用，用0清空节点所占内存去，并返回
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {	// 若有其他程序引用，则出错
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}	
	if (inode->i_nlinks)	// 若文件目录项连接数不为0,则表示还有其他文件目录项使用该节点，不应释放
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))	// 取i节点所在设备的超级块，测试设备是否存在
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)	// 如果i节点号小于1或大于该设备上i节点总数，则出错(0号i节点保留未使用)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))		// 该i节点对应的节点位图不存在
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))	// 复位i节点对应节点位图中的比特位，如果已经等于0,则出错
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;		// 置i节点位图所在缓冲区已修改标志，并清空该iridian结构所占内存区
	memset(inode,0,sizeof(*inode));
}
// 为设备建立一个新i节点，返回其指针。
// 在内存i节点表中获取一个空闲i节点表项，并从i节点位图中找一个空闲i节点
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;
// 从内存i节点表中获取一个空闲i节点项
	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))		// 读取指定设备的超级块结构
		panic("new_inode with unknown device");
	j = 8192;	// 扫描i节点位图，寻找第一个0位，寻找空闲节点，获取放置该i节点的节点号
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	// 若全部扫描完还没找到，或位图所在缓冲块无效，则退出
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	// 置对应新i节点的i节点位图位，如果已置位，则出错
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;	// 置i节点位图所在缓冲区已修改标志
	// 初始化i节点结构
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
