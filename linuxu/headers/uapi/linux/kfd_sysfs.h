/* linuxu: SHIM (upstream amdkfd includes <uapi/linux/kfd_sysfs.h> directly;
 * the vendor linuxu tree carries the identical pure-constants header at
 * linuxu/headers/linux/kfd_sysfs.h — reuse it here so the unmodified
 * .c compiles. Content is byte-for-byte the uapi constants file. */
#include <linux/kfd_sysfs.h>
