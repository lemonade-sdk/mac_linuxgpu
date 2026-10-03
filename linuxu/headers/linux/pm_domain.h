/* linuxu: SHIM (third_party/linux/include/linux/pm_domain.h)
 *
 * Generic PM domain API surface. Only used by amdgpu_acp.c (which is
 * excluded from the build by CONFIG_DRM_AMD_ACP=n, but the header must
 * exist for the include). Minimal SHIM.
 */
#ifndef __LINUX_PM_DOMAIN_H
#define __LINUX_PM_DOMAIN_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
/* workqueue.h cannot be included here: it pulls wait.h -> jiffies.h ->
 * kernel.h -> sched.h -> mm.h -> ... -> device.h -> pm_domain.h, so the
 * work_struct type must come from a forward declaration instead. The
 * embedded power_off_work is never dispatched by the shim. */
struct work_struct;

struct device;
struct dev_pm_domain {
	int (*power_on)(struct device *dev);
	int (*power_off)(struct device *dev);
	int (*power_off_prepare)(struct device *dev);
	int (*power_on_complete)(struct device *dev);
};

enum gpd_status {
	GPD_STATE_INVALID,
	GPD_STATE_ACTIVE,
	GPD_STATE_PREPARE_OFF,
	GPD_STATE_SUSPENDED,
	GPD_STATE_OFF,
};

struct dev_power_governor;
struct genpd_governor_data;
struct fwnode_handle;
struct opp_table;
struct gpd_dev_ops;

struct generic_pm_domain {
	struct device *dev;
	struct dev_pm_domain domain;
	struct list_head gpd_list_node;
	struct list_head parent_links;
	struct list_head child_links;
	struct list_head dev_list;
	struct dev_power_governor *gov;
	struct genpd_governor_data *gd;
	/* shim: no workqueue dispatch in this header cycle; reserved slot */
	void *power_off_work;
	struct fwnode_handle *provider;
	bool has_provider;
	const char *name;
	atomic_t sd_count;
	enum gpd_status status;
	unsigned int device_count;
	unsigned int device_id;
	unsigned int suspended_count;
	unsigned int prepared_count;
	unsigned int performance_state;
	bool cpus;
	bool synced_poweroff;
	bool stay_on;
	int (*power_off)(struct generic_pm_domain *domain);
	int (*power_on)(struct generic_pm_domain *domain);
};

/* ---- runtime (linuxu/src/sync/pm_domain.c, or no-op) ---- */
extern void pm_genpd_init(struct generic_pm_domain *genpd,
			  struct dev_pm_domain *ops, bool is_off);
extern int  pm_genpd_add_device(struct generic_pm_domain *genpd,
				struct device *dev);
extern int  pm_genpd_remove_device(struct device *dev);

#endif /* __LINUX_PM_DOMAIN_H */
