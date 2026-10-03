/* linuxu: SHIM (third_party/linux/include/linux/io-64-nonatomic-lo-hi.h)
 *
 * The host shim already has atomic 64-bit readq/writeq in <linux/io.h>,
 * so this header is an empty passthrough (all the vendor #ifndef'd
 * definitions resolve to the io.h versions).
 */
#ifndef _LINUX_IO_64_NONATOMIC_LO_HI_H_
#define _LINUX_IO_64_NONATOMIC_LO_HI_H_

#include <linux/io.h>

#endif	/* _LINUX_IO_64_NONATOMIC_LO_HI_H_ */
