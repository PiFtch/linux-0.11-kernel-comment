#ifndef _HEAD_H
#define _HEAD_H

typedef struct desc_struct {	// 段描述符数据结构，每个描述符8字节，每个描述符表256项。
	unsigned long a,b;
} desc_table[256];

extern unsigned long pg_dir[1024];	// 内存页目录数组。每个目录项4字节，从物理地址0开始。
extern desc_table idt,gdt;			// 中断描述符表，全局描述符表

#define GDT_NUL 0		// 全局描述符表的第0项，不用
#define GDT_CODE 1		// 第1项，内核代码段描述符项
#define GDT_DATA 2
#define GDT_TMP 3		// 第3项，系统段描述符，linux未使用

#define LDT_NUL 0		// 每个局部描述符表的第0项，不用
#define LDT_CODE 1		// 用户程序代码段描述符项
#define LDT_DATA 2

#endif
