/* linuxu: SHIM (third_party/linux/include/linux/memory.h)
 *
 * NUMA memory topology — the kfd_events.c include is vestigial (no
 * memory_group use in the driver); a minimal header suffices. */
#ifndef _LINUX_MEMORY_H_
#define _LINUX_MEMORY_H_

#define SECTION_SIZE_BITS	27
#define MIN_MEMORY_BLOCK_SIZE	(1UL << SECTION_SIZE_BITS)

#endif
