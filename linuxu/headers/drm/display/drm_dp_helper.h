/* linuxu: EDITED (third_party/linux/include/drm/display/drm_dp_helper.h) —
 * struct drm_dp_aux / drm_dp_aux_msg carried verbatim (atombios_dp.c
 * fills the transfer callback and reads aux->ddc / aux->dev /
 * aux->drm_dev), plus the DPCD access + link-training helpers amdgpu
 * actually calls.  The i2c_adapter member is kept as the linuxu
 * <linux/i2c.h> shim. */
/*
 * Copyright © 2008 Keith Packard
 * (upstream header, see third_party/linux/include/drm/display/drm_dp_helper.h)
 */

#ifndef _DRM_DP_HELPER_H_
#define _DRM_DP_HELPER_H_

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/types.h>

#include <drm/display/drm_dp.h>
#include <drm/drm_connector.h>

struct drm_device;
struct drm_dp_aux;
struct drm_panel;

bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			  int lane_count);
bool drm_dp_clock_recovery_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			      int lane_count);
bool drm_dp_post_lt_adj_req_in_progress(const u8 link_status[DP_LINK_STATUS_SIZE]);
u8 drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE],
				     int lane);
u8 drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane);
u8 drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE],
				   int lane);

int drm_dp_read_clock_recovery_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     enum drm_dp_phy dp_phy, bool uhbr);
int drm_dp_read_channel_eq_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				 enum drm_dp_phy dp_phy, bool uhbr);

void drm_dp_link_train_clock_recovery_delay(const struct drm_dp_aux *aux,
					    const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
void drm_dp_lttpr_link_train_clock_recovery_delay(void);
void drm_dp_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
void drm_dp_lttpr_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					      const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);

int drm_dp_128b132b_read_aux_rd_interval(struct drm_dp_aux *aux);
bool drm_dp_128b132b_lane_channel_eq_done(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane_count);
bool drm_dp_128b132b_lane_symbol_locked(const u8 link_status[DP_LINK_STATUS_SIZE],
					int lane_count);
bool drm_dp_128b132b_eq_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE]);
bool drm_dp_128b132b_cds_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE]);
bool drm_dp_128b132b_link_training_failed(const u8 link_status[DP_LINK_STATUS_SIZE]);

u8 drm_dp_link_rate_to_bw_code(int link_rate);
int drm_dp_bw_code_to_link_rate(u8 link_bw);

const char *drm_dp_phy_name(enum drm_dp_phy dp_phy);

/**
 * struct drm_dp_aux_msg - DisplayPort AUX channel transaction
 *
 * This structure represents a DisplayPort AUX channel transaction.
 */
struct drm_dp_aux_msg {
	/**
	 * @address: byte address of the start of the DPCD register to be
	 * read or written
	 */
	unsigned int address;

	/**
	 * @data: pointer to the buffer to be written
	 */
	u8 *data;

	/**
	 * @reply: reply buffer for the AUX channel transaction
	 */
	u8 reply;

	/**
	 * @size: the number of bytes to be transferred
	 */
	size_t size;

	/**
	 * @rx_len: the number of bytes received
	 */
	int rx_len;

	/**
	 * @flags: a combination of DP_AUX_* flags
	 */
	unsigned int flags;

	/*
	 * linuxu: SHIM — legacy (pre-2026) members used by this fork's
	 * atombios_dp.c / amdgpu_dm_mst_types.c (request byte + data buffer
	 * alias); the 2026 vendor layout has `request` separately and uses
	 * `data`. Both spellings must be legal for the unmodified driver.
	 */
	unsigned int request;
	u8 *buffer;
	u8 reply_buf[2];
#define reply reply_buf[0]
};

/**
 * struct drm_dp_aux_cec - DisplayPort CEC-Tunneling-over-AUX
 *
 * This structure contains information required for CEC tunneling over AUX
 * as described in DisplayPort specification.
 */
struct drm_dp_aux_cec {
	/**
	 * @cec_rx_irq_pending: Flag indicating whether a CEC RX interrupt
	 * is pending and needs to be processed.
	 */
	bool cec_rx_irq_pending;

	/**
	 * @cec_tx_irq_pending: Flag indicating whether a CEC TX interrupt
	 * is pending and needs to be processed.
	 */
	bool cec_tx_irq_pending;

	/**
	 * @cec_irq: The IRQ number for CEC tunneling.
	 */
	unsigned int cec_irq;
};

/**
 * struct drm_dp_aux - DisplayPort AUX channel
 *
 * This structure represents a DisplayPort AUX channel.
 *
 * An AUX channel is a bidirectional data channel for carrying
 * transactions between a display and its source device.
 */
struct drm_dp_aux {
	/**
	 * @name: user-visible name of this AUX channel and the
	 * I2C-over-AUX adapter.
	 */
	const char *name;

	/**
	 * @ddc: I2C adapter that can be used for I2C-over-AUX
	 * communication
	 */
	struct i2c_adapter ddc;

	/**
	 * @dev: pointer to struct device that is the parent for this
	 * AUX channel.
	 */
	struct device *dev;

	/**
	 * @drm_dev: pointer to the &drm_device that owns this AUX channel.
	 */
	struct drm_device *drm_dev;

	/**
	 * @crtc: backpointer to the crtc that is currently using this
	 * AUX channel
	 */
	struct drm_crtc *crtc;

	/**
	 * @hw_mutex: internal mutex used for locking transfers.
	 */
	struct mutex hw_mutex;

	/**
	 * @crc_work: worker that captures CRCs for each frame
	 */
	struct work_struct crc_work;

	/**
	 * @crc_count: counter of captured frame CRCs
	 */
	u8 crc_count;

	/**
	 * @transfer: transfers a message representing a single AUX
	 * transaction (driver-provided callback).
	 */
	ssize_t (*transfer)(struct drm_dp_aux *aux,
			    struct drm_dp_aux_msg *msg);

	/**
	 * @wait_hpd_asserted: wait for HPD to be asserted
	 */
	int (*wait_hpd_asserted)(struct drm_dp_aux *aux, unsigned long wait_us);

	/**
	 * @i2c_nack_count: Counts I2C NACKs, used for DP validation.
	 */
	unsigned i2c_nack_count;

	/**
	 * @i2c_defer_count: Counts I2C DEFERs, used for DP validation.
	 */
	unsigned i2c_defer_count;

	/**
	 * @cec: struct containing fields used for CEC-Tunneling-over-AUX.
	 */
	struct drm_dp_aux_cec cec;

	/**
	 * @is_remote: Is this AUX CH actually using sideband messaging.
	 */
	bool is_remote;
};

/**
 * drm_dp_aux_init - initialise an AUX channel
 */
void drm_dp_aux_init(struct drm_dp_aux *aux);

/**
 * drm_dp_aux_register - register an AUX channel
 */
int drm_dp_aux_register(struct drm_dp_aux *aux);

/**
 * drm_dp_aux_unregister - unregister an AUX channel
 */
void drm_dp_aux_unregister(struct drm_dp_aux *aux);

/**
 * drm_dp_dpcd_read - read DPCD data over the AUX channel
 */
ssize_t drm_dp_dpcd_read(struct drm_dp_aux *aux, unsigned int offset,
			 void *val, size_t size);

/**
 * drm_dp_dpcd_write - write DPCD data over the AUX channel
 */
ssize_t drm_dp_dpcd_write(struct drm_dp_aux *aux, unsigned int offset,
			  const void *val, size_t size);

/**
 * drm_dp_dpcd_readb() - read a single byte from the DPCD
 */
static inline ssize_t drm_dp_dpcd_readb(struct drm_dp_aux *aux,
					unsigned int offset,
					u8 *valuep)
{
	return drm_dp_dpcd_read(aux, offset, valuep, 1);
}

/**
 * drm_dp_dpcd_read_data() - read a series of bytes from the DPCD
 */
static inline int drm_dp_dpcd_read_data(struct drm_dp_aux *aux,
					unsigned int offset, u8 *buffer,
					size_t size)
{
	ssize_t ret;

	ret = drm_dp_dpcd_read(aux, offset, buffer, size);
	if (ret < 0)
		return ret;
	if ((size_t)ret != size)
		return -EPROTO;
	return 0;
}

/**
 * drm_dp_dpcd_write_data() - write a series of bytes to the DPCD
 */
static inline int drm_dp_dpcd_write_data(struct drm_dp_aux *aux,
					 unsigned int offset, const u8 *buffer,
					 size_t size)
{
	ssize_t ret;

	ret = drm_dp_dpcd_write(aux, offset, buffer, size);
	if (ret < 0)
		return ret;
	if ((size_t)ret != size)
		return -EPROTO;
	return 0;
}

/**
 * drm_dp_dpcd_writeb() - write a single byte to the DPCD
 */
static inline ssize_t drm_dp_dpcd_writeb(struct drm_dp_aux *aux,
					 unsigned int offset, u8 value)
{
	return drm_dp_dpcd_write(aux, offset, &value, 1);
}

/**
 * drm_dp_dpcd_read_byte() - read a single byte from the DPCD
 */
static inline int drm_dp_dpcd_read_byte(struct drm_dp_aux *aux,
					unsigned int offset,
					u8 *valuep)
{
	return drm_dp_dpcd_read_data(aux, offset, valuep, 1);
}

/**
 * drm_dp_dpcd_write_byte() - write a single byte to the DPCD
 */
static inline int drm_dp_dpcd_write_byte(struct drm_dp_aux *aux,
					 unsigned int offset, u8 value)
{
	return drm_dp_dpcd_write_data(aux, offset, &value, 1);
}

/**
 * drm_dp_dpcd_read_link_status - read the link status registers
 */
int drm_dp_dpcd_read_link_status(struct drm_dp_aux *aux,
				 u8 *link_status);

/**
 * drm_dp_dpcd_write_link_status - write the link status registers
 */
int drm_dp_dpcd_write_link_status(struct drm_dp_aux *aux,
				  const u8 *link_status);

/**
 * drm_dp_max_link_rate - the maximum link rate in kHz from the DPCD
 */
static inline u8
drm_dp_max_link_rate(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return (dpcd[DP_DPCD_MAX_LINK_RATE] & 0x0f);
}

/**
 * drm_dp_max_lane_count - the maximum number of lanes from the DPCD
 */
static inline u8
drm_dp_max_lane_count(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return (dpcd[DP_DPCD_MAX_LANE_COUNT_SUPPORTED] & 0x07) + 1;
}

/**
 * drm_dp_enhanced_frame_cap - the enhanced frame cap from the DPCD
 */
static inline bool
drm_dp_enhanced_frame_cap(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return (dpcd[DP_DPCD_MAX_PBN] & DP_ENHANCED_FRAME_CAP);
}

void drm_dp_set_subconnector_property(struct drm_connector *connector,
				      enum drm_connector_status status,
				      const u8 *dpcd,
				      const u8 *downstream_ports);

#endif
