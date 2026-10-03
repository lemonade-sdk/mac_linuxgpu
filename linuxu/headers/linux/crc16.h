/* linuxu: SHIM (third_party/linux/include/linux/crc16.h) — copied verbatim. */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CRC16_H
#define __CRC16_H

#include <linux/types.h>

u16 crc16(u16 crc, const u8 *p, size_t len);

#endif /* __CRC16_H */
