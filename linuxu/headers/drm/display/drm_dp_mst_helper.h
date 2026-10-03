/* linuxu: EDITED (third_party/linux/include/drm/display/drm_dp_mst_helper.h)
 * — struct drm_dp_mst_topology_mgr / drm_dp_mst_branch carried verbatim
 * (struct amdgpu_mst_connector embeds the mgr; the KMD only stores it,
 * the MST engine never runs in the compute dext).  Kernel-only includes
 * (stackdepot.h, timekeeping.h) and the MST engine prototypes are
 * trimmed to what the KMD references. */
#ifndef __DRM_DP_MST_HELPER_H
#define __DRM_DP_MST_HELPER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/dma-fence.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_dp_mst_const.h>
#include <drm/drm_atomic.h>
#include <drm/drm_fixed.h>

/*
 * struct drm_private_obj/drm_private_state follow upstream (vendor
 * drm/drm_atomic.h): the driver's dm/ code subclasses them and the
 * full 2026 layout is required. (An older fork-local 3-field variant
 * lived here; it conflicted with the canonical definitions.)
 */
struct drm_private_state;
struct drm_private_state_funcs;

struct drm_dp_mst_branch;

struct drm_dp_mst_topo_pipe {
	struct drm_dp_mst_branch *dst;
	uint8_t link_rate;
	uint8_t lane_count;
	uint8_t pbn;
	uint8_t slot_start;
	uint8_t slot_count;
	uint8_t mstb_port_num;
	uint8_t virtual_port;
	bool busy;
};

/**
 * struct drm_dp_sideband_msg_rx - Sideband message receiver
 */
struct drm_dp_sideband_msg_rx {
	struct delayed_work work;
	struct list_head tx_msg_downq;
	bool pending;
	bool rx_state;
	uint8_t data[DP_SIDEBAND_MSG_SIZE];
	size_t len;
};

/**
 * struct drm_dp_mst_branch - MST branch device
 */
struct drm_dp_mst_branch {
	/** @mstb: backpointer to the parent mstb */
	struct drm_dp_mst_branch *mstb;
	/** @parent: the parent branch */
	struct drm_dp_mst_branch *parent;
	/** @port_num: the port number on the parent */
	int port_num;
	/** @virtual_port: the virtual port number */
	int virtual_port;
	/** @in_dpcd: the input DPCD */
	uint8_t in_dpcd[DP_RECEIVER_CAP_SIZE];
	/** @out_dpcd: the output DPCD */
	uint8_t out_dpcd[DP_RECEIVER_CAP_SIZE];
	/** @dpcd_rev: the DPCD revision */
	int dpcd_rev;
	/** @max_lanes: max number of lanes */
	int max_lanes;
	/** @max_link_rate: max link rate */
	int max_link_rate;
	/** @base_rate: base link rate */
	int base_rate;
	/** @sink_count: number of sinks */
	int sink_count;
	/** @num_ports: number of ports */
	int num_ports;
	/** @link_rate: current link rate */
	int link_rate;
	/** @lane_count: current lane count */
	int lane_count;
	/** @pbn: max PBN */
	int pbn;
	/** @slot_count: number of slots */
	int slot_count;
	/** @slot_start: start slot */
	int slot_start;
	/** @mstb_port_num: the mstb port number */
	int mstb_port_num;
	/** @busy: is the branch busy */
	bool busy;
	/** @in_use: is the branch in use */
	bool in_use;
	/** @in_band_msg: in-band message */
	int in_band_msg;
	/** @mst_primary: pointer to the primary mstb */
	struct drm_dp_mst_branch *mst_primary;
	/** @children: array of child branches */
	struct drm_dp_mst_branch *children[DP_MAX_LANES];
	/** @pools: array of PBN pools */
	struct drm_dp_mst_branch *pools[DP_MAX_PBN];
	/** @mgr: backpointer to the topology manager */
	struct drm_dp_mst_topology_mgr *mgr;
	/** @dev: the drm device */
	struct drm_device *dev;
	/** @aux: the DP aux channel */
	struct drm_dp_aux *aux;
	/** @branch_id: the branch id */
	int branch_id;
	/** @lock: the branch lock */
	struct mutex lock;
	/** @probe_work: the probe work */
	struct delayed_work probe_work;
	/** @up_req_recv: the upstream request receiver */
	struct drm_dp_sideband_msg_rx up_req_recv;
	/** @down_rep_recv: the downstream reply receiver */
	struct drm_dp_sideband_msg_rx down_rep_recv;
};

/**
 * struct drm_dp_mst_topology_cbs - MST topology callbacks
 */
struct drm_dp_mst_topology_cbs {
	/** @atomic_check: atomic check callback */
	int (*atomic_check)(struct drm_encoder *encoder,
			    struct drm_atomic_state *state);
	/** @mode_valid: mode valid callback */
	enum drm_mode_status (*mode_valid)(struct drm_encoder *encoder,
					   const struct drm_display_mode *mode);
	/** @update_bw: update bandwidth callback */
	int (*update_bw)(struct drm_encoder *encoder,
			 struct drm_atomic_state *state);
	/** @disable: disable callback */
	void (*disable)(struct drm_encoder *encoder);
	/** @enable: enable callback */
	void (*enable)(struct drm_encoder *encoder);
	/** @post_disable: post disable callback */
	void (*post_disable)(struct drm_encoder *encoder);
	/** @destroy: destroy callback */
	void (*destroy)(struct drm_encoder *encoder);
	/** @reset: reset callback */
	void (*reset)(struct drm_encoder *encoder);
};

/**
 * struct drm_dp_mst_topology_mgr - MST topology manager
 *
 * Carried with the upstream field order for the fields the KMD stores
 * (it embeds the struct in amdgpu_mst_connector; the compute dext never
 * drives the MST engine, so only storage matters).
 */
struct drm_dp_mst_topology_mgr {
	/** @base: the private object */
	struct drm_private_obj base;
	/** @dev: the drm device */
	struct drm_device *dev;
	/** @cbs: the topology callbacks */
	const struct drm_dp_mst_topology_cbs *cbs;
	/** @max_dpcd_transaction_bytes: max DPCD transaction bytes */
	int max_dpcd_transaction_bytes;
	/** @aux: the DP aux channel */
	struct drm_dp_aux *aux;
	/** @max_payloads: max number of payloads */
	int max_payloads;
	/** @conn_base_id: the base connector id */
	int conn_base_id;
	/** @up_req_recv: the upstream request receiver */
	struct drm_dp_sideband_msg_rx up_req_recv;
	/** @down_rep_recv: the downstream reply receiver */
	struct drm_dp_sideband_msg_rx down_rep_recv;
	/** @lock: the manager lock */
	struct mutex lock;
	/** @probe_lock: the probe lock */
	struct mutex probe_lock;
	/** @mst_state: MST state */
	bool mst_state : 1;
	/** @payload_id_table_cleared: payload id table cleared */
	bool payload_id_table_cleared : 1;
	/** @reset_rx_state: reset RX state */
	bool reset_rx_state : 1;
	/** @payload_count: number of payloads */
	u8 payload_count;
	/** @next_start_slot: next start slot */
	u8 next_start_slot;
	/** @mst_primary: the primary branch */
	struct drm_dp_mst_branch *mst_primary;
	/** @dpcd: the DPCD */
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	/** @sink_count: number of sinks */
	u8 sink_count;
	/** @funcs: the private state functions */
	const struct drm_private_state_funcs *funcs;
	/** @qlock: the queue lock */
	struct mutex qlock;
	/** @tx_msg_downq: the downstream TX message queue */
	struct list_head tx_msg_downq;
	/** @tx_msg_up_req: the upstream TX message queue */
	struct list_head tx_msg_up_req;
	/** @up_req_msg: the upstream request message */
	uint8_t up_req_msg[DP_SIDEBAND_MSG_SIZE];
	/** @down_rep_msg: the downstream reply message */
	uint8_t down_rep_msg[DP_SIDEBAND_MSG_SIZE];
	/** @down_rep_msg_len: the downstream reply message length */
	size_t down_rep_msg_len;
	/** @up_req_msg_len: the upstream request message length */
	size_t up_req_msg_len;
	/** @probe_work: the probe work */
	struct delayed_work probe_work;
	/** @work: the manager work */
	struct work_struct work;
	/** @base_id: the base id */
	int base_id;
	/** @conn_base_id: the base connector id (duplicate) */
	int conn_base_id_2;
	/** @payload_id_table: the payload id table */
	u8 payload_id_table[DP_MAX_PAYLOADS];
	/** @slot_count: the slot count */
	int slot_count;
	/** @link_rate: the link rate */
	int link_rate;
	/** @lane_count: the lane count */
	int lane_count;
	/** @base_rate: the base rate */
	int base_rate;
	/** @pbn: the PBN */
	int pbn;
	/** @in_use: in use */
	bool in_use;
	/** @busy: busy */
	bool busy;
	/** @virtual_port: the virtual port */
	int virtual_port;
	/** @mstb_port_num: the mstb port number */
	int mstb_port_num;
	/** @slot_start: the start slot */
	int slot_start;
	/** @busy: busy (duplicate) */
	bool busy_2;
};

/**
 * struct drm_dp_mst_port - MST port
 */
struct drm_dp_mst_port {
	/** @connector: the connector */
	struct drm_connector *connector;
	/** @virtual_port: the virtual port */
	u8 virtual_port;
	/** @mstb: the branch */
	struct drm_dp_mst_branch *mstb;
	/** @mgr: the manager */
	struct drm_dp_mst_topology_mgr *mgr;
	/** @busy: busy */
	bool busy;
	/** @in_use: in use */
	bool in_use;
	/** @pbn: the PBN */
	int pbn;
	/** @slot_count: the slot count */
	int slot_count;
	/** @slot_start: the start slot */
	int slot_start;
	/** @link_rate: the link rate */
	int link_rate;
	/** @lane_count: the lane count */
	int lane_count;
	/** @base_rate: the base rate */
	int base_rate;
	/** @port_num: the port number */
	int port_num;
	/** @mstb_port_num: the mstb port number */
	int mstb_port_num;
	/** @busy: busy (duplicate) */
	bool busy_2;
};

/*
 * MST engine entry points — defined in src/drm/drm_core.c as no-ops for
 * the compute build (the KMD only stores the mgr struct; it never calls
 * these in a compute-only configuration).
 */
int drm_dp_mst_topology_mgr_init(struct drm_dp_mst_topology_mgr *mgr,
				 struct drm_device *dev,
				 int max_dpcd_transaction_bytes,
				 int max_payloads, int conn_base_id,
				 const struct drm_dp_mst_topology_cbs *cbs);
void drm_dp_mst_topology_mgr_destroy(struct drm_dp_mst_topology_mgr *mgr);
int drm_dp_mst_topology_mgr_probe(struct drm_dp_mst_topology_mgr *mgr);
void drm_dp_mst_topology_mgr_unplug(struct drm_dp_mst_topology_mgr *mgr);
int drm_dp_mst_register_encoder(struct drm_dp_mst_topology_mgr *mgr,
				struct drm_encoder *encoder);
void drm_dp_mst_unregister_encoder(struct drm_dp_mst_topology_mgr *mgr,
				   struct drm_encoder *encoder);
int drm_dp_mst_add_port(struct drm_dp_mst_topology_mgr *mgr,
			struct drm_dp_mst_port *port);
void drm_dp_mst_del_port(struct drm_dp_mst_topology_mgr *mgr,
			 struct drm_dp_mst_port *port);
void drm_dp_mst_hotplug(struct drm_dp_mst_topology_mgr *mgr);
int drm_dp_mst_hpd_handler(struct drm_dp_mst_topology_mgr *mgr);

#endif
