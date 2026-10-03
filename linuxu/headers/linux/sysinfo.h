/* linuxu: SHIM (third_party/linux/include/linux/sysinfo.h) */
#ifndef __LINUX_SYSINFO_H
#define __LINUX_SYSINFO_H

#include <linux/types.h>

struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;
	unsigned short pad;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
	char _f[20-2*sizeof(long)-sizeof(int)];
};

extern int si_meminfo(struct sysinfo *info);
/* Preflight before upstream entry points that cannot propagate query errors. */
extern int linuxu_sysinfo_init(void);

#endif /* __LINUX_SYSINFO_H */
