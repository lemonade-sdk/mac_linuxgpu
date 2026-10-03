/* linuxu shim: mutex — see linuxu/src/sync.c (pthread mutex in a
 * side table, matching the fixed struct mutex layout).  This file
 * anchors the namespace. */
#include <linux/mutex.h>

void linuxu_mutex_anchor(void)
{
}
