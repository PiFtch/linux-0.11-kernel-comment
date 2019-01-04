/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

// 数据块写--想指定设备从给定偏移处写入指定长度数据，count为要传送的字节数
// 返回已写入字节数，若没有写入任何字节或出错，则返回出错号
// 对诶核老说，写操作是向高速缓冲区写入数据，最终写入设备由高速缓冲管理程序决堤个并处理
// 如果写开始位置不处于块起始处，要先将开始字节所在的整个块独处，然后写入数据，再将完整一块写盘
int block_write(int dev, long * pos, char * buf, int count)
{// 首先将pos换算成开始读写盘块的块序号block，并丘处需写第一字节在该块中的偏移offset
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int written = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE - offset;
		if (chars > count)
			chars=count;
		if (chars == BLOCK_SIZE)
			bh = getblk(dev,block);
		else
			bh = breada(dev,block,block+1,block+2,-1);
		block++;
		if (!bh)
			return written?written:-EIO;
// 接着吧指针p指向读出数据的缓冲块中开始写入数据的位置，若最后一次循环写入的数据不足一块
// 需从块开始处天蝎（修改）所需的字节，因此这里需要预先设置offset为0，此后将文件中偏移指针pos前移此次将要写的字节数chars，
// 并累加这些要写的字节数到统计值written中，再把还需要写的计数值count减去此次要写的chars，
// 然后从用户缓冲区复制chars个自负要p指向的高速缓冲块中开始写入的位置处，复制完后
// 就设置该缓冲块已修改标志，并释放该缓冲区（引用计数-1）
		p = offset + bh->b_data;
		offset = 0;
		*pos += chars;
		written += chars;
		count -= chars;
		while (chars-->0)
			*(p++) = get_fs_byte(buf++);
		bh->b_dirt = 1;
		brelse(bh);
	}
	return written;
}

int block_read(int dev, unsigned long * pos, char * buf, int count)
{
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int read = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE-offset;
		if (chars > count)
			chars = count;
		if (!(bh = breada(dev,block,block+1,block+2,-1)))
			return read?read:-EIO;
		block++;
		p = offset + bh->b_data;
		offset = 0;
		*pos += chars;
		read += chars;
		count -= chars;
		while (chars-->0)
			put_fs_byte(*(p++),buf++);
		brelse(bh);
	}
	return read;
}
