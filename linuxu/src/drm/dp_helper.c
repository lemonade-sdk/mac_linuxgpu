/* linuxu shim: pure DisplayPort link-training arithmetic and the non-x86
 * panel orientation quirk, taken from the pinned
 * drivers/gpu/drm/display/drm_dp_helper.c and
 * drivers/gpu/drm/drm_panel_orientation_quirks.c. These helpers only decode
 * DPCD bytes or sleep; AUX transfers and connector state are not provided.
 * The upstream drm_dbg_kms() for an out-of-range AUX interval is omitted. */
#include <linux/types.h>
#include <linux/delay.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_utils.h>

static u8 dp_link_status(const u8 link_status[DP_LINK_STATUS_SIZE], int r)
{
	return link_status[r - DP_LANE0_1_STATUS];
}

static u8 dp_get_lane_status(const u8 link_status[DP_LINK_STATUS_SIZE],
			     int lane)
{
	int i = DP_LANE0_1_STATUS + (lane >> 1);
	int s = (lane & 1) * 4;
	u8 l = dp_link_status(link_status, i);

	return (l >> s) & 0xf;
}

bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			  int lane_count)
{
	u8 lane_align;
	u8 lane_status;
	int lane;

	lane_align = dp_link_status(link_status,
				    DP_LANE_ALIGN_STATUS_UPDATED);
	if ((lane_align & DP_INTERLANE_ALIGN_DONE) == 0)
		return false;
	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_CHANNEL_EQ_BITS) != DP_CHANNEL_EQ_BITS)
			return false;
	}
	return true;
}

bool drm_dp_clock_recovery_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			      int lane_count)
{
	int lane;
	u8 lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_LANE_CR_DONE) == 0)
			return false;
	}
	return true;
}

u8 drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE],
				     int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT :
		 DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
}

u8 drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT :
		 DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
}

static int __8b10b_clock_recovery_delay_us(u8 rd_interval)
{
	if (rd_interval == 0)
		return 100;

	return rd_interval * 4 * 1000;
}

static int __8b10b_channel_eq_delay_us(u8 rd_interval)
{
	if (rd_interval == 0)
		return 400;

	return rd_interval * 4 * 1000;
}

void drm_dp_link_train_clock_recovery_delay(const struct drm_dp_aux *aux,
					    const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 rd_interval = dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
		DP_TRAINING_AUX_RD_MASK;
	int delay_us;

	(void)aux;
	if (dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
		delay_us = 100;
	else
		delay_us = __8b10b_clock_recovery_delay_us(rd_interval);

	usleep_range(delay_us, delay_us * 2);
}

void drm_dp_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	int delay_us = __8b10b_channel_eq_delay_us(dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
						   DP_TRAINING_AUX_RD_MASK);

	(void)aux;
	usleep_range(delay_us, delay_us * 2);
}

u8 drm_dp_link_rate_to_bw_code(int link_rate)
{
	switch (link_rate) {
	case 1000000:
		return DP_LINK_BW_10;
	case 1350000:
		return DP_LINK_BW_13_5;
	case 2000000:
		return DP_LINK_BW_20;
	default:
		/* Spec says link_bw = link_rate / 0.27Gbps */
		return link_rate / 27000;
	}
}

/* drm_panel_orientation_quirks.c, !CONFIG_X86 branch: no quirks exist for
 * non-x86 devices. */
int drm_get_panel_orientation_quirk(int width, int height)
{
	(void)width; (void)height;
	return DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
}
