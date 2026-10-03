/* linuxu shim: completion — see linuxu/src/sync.c; the completion
 * family lives there (single side-table for all condvar-backed
 * primitives).  This file only anchors the namespace. */
#include <linux/completion.h>

void linuxu_completion_anchor(void)
{
}
