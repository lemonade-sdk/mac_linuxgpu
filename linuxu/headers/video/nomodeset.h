/* linuxu: SHIM (third_party/linux/include/video/nomodeset.h)
 * The nomodeset flag.
 * Runtime: linuxu/src/shims/nomodeset.c
 */
#include <stdbool.h>

#ifndef _VIDEO_NOMODESET_H
#define _VIDEO_NOMODESET_H

extern bool nomodeset;
extern bool video_firmware_drivers_only(void);

#endif /* _VIDEO_NOMODESET_H */
