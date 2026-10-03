/* linuxu: AS-IS — maps to libc <stddef.h> */
#ifndef _LINUXU_LIBC_STDDEF_H
#define _LINUXU_LIBC_STDDEF_H
#include <stddef.h>
#endif

/* linuxu: canonical container_of (upstream linux/stddef.h) */
#ifndef __container_of
#define __container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#ifndef container_of
#define container_of(ptr, type, member) __container_of(ptr, type, member)
#endif
