/* linuxu: EDITED (third_party/linux/include/media/cec.h) - CONFIG_CEC_CORE=n:
 * only the upstream !IS_REACHABLE(CONFIG_CEC_CORE) API and the trailing
 * inline helpers are kept; the adapter framework structs (and their
 * cdev/kthread/rc-core includes) are dropped because no CEC adapter
 * exists in this build. */
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cec - HDMI Consumer Electronics Control support header
 *
 * Copyright 2016 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#ifndef _MEDIA_CEC_H
#define _MEDIA_CEC_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/cec.h>

struct cec_adapter;
struct device;
struct edid;
struct drm_connector;

#if IS_REACHABLE(CONFIG_CEC_CORE)
#error "linuxu builds CEC_CORE=n"
#else


static inline int cec_register_adapter(struct cec_adapter *adap,
				       struct device *parent)
{
	return 0;
}

static inline void cec_unregister_adapter(struct cec_adapter *adap)
{
}

static inline void cec_delete_adapter(struct cec_adapter *adap)
{
}

static inline void cec_s_phys_addr(struct cec_adapter *adap, u16 phys_addr,
				   bool block)
{
}

static inline void cec_s_phys_addr_from_edid(struct cec_adapter *adap,
					     const struct edid *edid)
{
}

static inline u16 cec_get_edid_phys_addr(const u8 *edid, unsigned int size,
					 unsigned int *offset)
{
	if (offset)
		*offset = 0;
	return CEC_PHYS_ADDR_INVALID;
}

static inline void cec_s_conn_info(struct cec_adapter *adap,
				   const struct cec_connector_info *conn_info)
{
}

static inline void
cec_fill_conn_info_from_drm(struct cec_connector_info *conn_info,
			    const struct drm_connector *connector)
{
	memset(conn_info, 0, sizeof(*conn_info));
}

#endif

/**
 * cec_phys_addr_invalidate() - set the physical address to INVALID
 *
 * @adap:	the CEC adapter
 *
 * This is a simple helper function to invalidate the physical
 * address.
 */
static inline void cec_phys_addr_invalidate(struct cec_adapter *adap)
{
	cec_s_phys_addr(adap, CEC_PHYS_ADDR_INVALID, false);
}

/**
 * cec_get_edid_spa_location() - find location of the Source Physical Address
 *
 * @edid: the EDID
 * @size: the size of the EDID
 *
 * This EDID is expected to be a CEA-861 compliant, which means that there are
 * at least two blocks and one or more of the extensions blocks are CEA-861
 * blocks.
 *
 * The returned location is guaranteed to be <= size-2.
 *
 * This is an inline function since it is used by both CEC and V4L2.
 * Ideally this would go in a module shared by both, but it is overkill to do
 * that for just a single function.
 */
static inline unsigned int cec_get_edid_spa_location(const u8 *edid,
						     unsigned int size)
{
	unsigned int blocks = size / 128;
	unsigned int block;
	u8 d;

	/* Sanity check: at least 2 blocks and a multiple of the block size */
	if (blocks < 2 || size % 128)
		return 0;

	/*
	 * If there are fewer extension blocks than the size, then update
	 * 'blocks'. It is allowed to have more extension blocks than the size,
	 * since some hardware can only read e.g. 256 bytes of the EDID, even
	 * though more blocks are present. The first CEA-861 extension block
	 * should normally be in block 1 anyway.
	 */
	if (edid[0x7e] + 1 < blocks)
		blocks = edid[0x7e] + 1;

	for (block = 1; block < blocks; block++) {
		unsigned int offset = block * 128;

		/* Skip any non-CEA-861 extension blocks */
		if (edid[offset] != 0x02 || edid[offset + 1] != 0x03)
			continue;

		/* search Vendor Specific Data Block (tag 3) */
		d = edid[offset + 2] & 0x7f;
		/* Check if there are Data Blocks */
		if (d <= 4)
			continue;
		if (d > 4) {
			unsigned int i = offset + 4;
			unsigned int end = offset + d;

			/* Note: 'end' is always < 'size' */
			do {
				u8 tag = edid[i] >> 5;
				u8 len = edid[i] & 0x1f;

				if (tag == 3 && len >= 5 && i + len <= end &&
				    edid[i + 1] == 0x03 &&
				    edid[i + 2] == 0x0c &&
				    edid[i + 3] == 0x00)
					return i + 4;
				i += len + 1;
			} while (i < end);
		}
	}
	return 0;
}

#endif /* _MEDIA_CEC_H */
