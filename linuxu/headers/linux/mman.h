/* linuxu: SHIM (third_party/linux/include/linux/mman.h) */
#ifndef __LINUX_MMAN_H
#define __LINUX_MMAN_H
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_32BIT 0x40
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_FAILED ((void *)-1)
#endif
