/* linuxu shim: rwlock — legacy rwlock_t ops live in
 * linuxu/src/sync.c (mapped onto the spinlock table).  This file
 * anchors the namespace. */
#include <linux/rwlock.h>

void linuxu_rwlock_anchor(void)
{
}
