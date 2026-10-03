/* linuxu: SHIM (third_party/linux/include/linux/poison.h) */
#ifndef _LINUX_POISON_H
#define _LINUX_POISON_H
#define POISON1	((void *)0x00100100)
#define POISON2	((void *)0x00200200)
#define LIST_POISON1 ((void *) 0x100)
#define LIST_POISON2 ((void *) 0x122)
#define RB_POISON	((void *) 0x600)
#define NULL_POISON	((void *) 0x600)
#define STRING_POISON	((void *) 0x5678)
#define IRQ_POISON	((void *) 0x43210)
#endif
