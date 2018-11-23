/*
 *  linux/kernel/mktime.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <time.h>

/*
 * This isn't the library routine, it is only used in the kernel.
 * as such, we don't care about years<1970 etc, but assume everything
 * is ok. Similarly, TZ etc is happily ignored. We just do everything
 * as easily as possible. Let's find something public for the library
 * routines (although I think minix times is public).
 */
/*
 * PS. I hate whoever though up the year 1970 - couldn't they have gotten
 * a leap-year instead? I also hate Gregorius, pope or no. I'm grumpy.
 */
#define MINUTE 60
#define HOUR (60*MINUTE)
#define DAY (24*HOUR)
#define YEAR (365*DAY)

/* interestingly, we assume leap-years */
static int month[12] = {	// 下面以年为界限，定义了每个月开始时的秒数时间数组。
	0,
	DAY*(31),
	DAY*(31+29),
	DAY*(31+29+31),
	DAY*(31+29+31+30),
	DAY*(31+29+31+30+31),
	DAY*(31+29+31+30+31+30),
	DAY*(31+29+31+30+31+30+31),
	DAY*(31+29+31+30+31+30+31+31),
	DAY*(31+29+31+30+31+30+31+31+30),
	DAY*(31+29+31+30+31+30+31+31+30+31),
	DAY*(31+29+31+30+31+30+31+31+30+31+30)
};
// 该函数计算从1970.1.1 0时起到开机当日经过的秒数，作为开机时间。
long kernel_mktime(struct tm * tm)
{
	long res;
	int year;

	year = tm->tm_year - 70;		// 1970到现在经过的年数
/* magic offsets (y+1) needed to get leapyears right.*/
	res = YEAR*year + DAY*((year+1)/4);	// 这些年经过的秒数时间+每个闰年时多1天的秒数时间，再加上当年到当月时的秒数
	res += month[tm->tm_mon];
/* and (y+2) here. If it wasn't a leap-year, we have to adjust */
	if (tm->tm_mon>1 && ((year+2)%4))
		res -= DAY;
	res += DAY*(tm->tm_mday-1);		// 再加上本月过去的天数的秒数时间。
	res += HOUR*tm->tm_hour;		// 当天过去的小时数的秒数时间。
	res += MINUTE*tm->tm_min;		// 1小时内过去的分钟数的秒数时间。
	res += tm->tm_sec;				// 1分钟内已过的秒数。
	return res;
}	// 如果y能被4整除且不能被100整除，或者能被400整除，则y是闰年。
