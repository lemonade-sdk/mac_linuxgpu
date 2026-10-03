/* linuxu: SHIM (third_party/linux/include/linux/acpi_amd_wbrf.h)
 * WBRF frequency-band-range structs (used by the SMU exclusion path) +
 * no-op ACPI notifier stubs (CONFIG_AMD_WBRF = n in this build). */
#ifndef __LINUX_ACPI_AMD_WBRF_H
#define __LINUX_ACPI_AMD_WBRF_H

#include <linux/types.h>
#include <stdbool.h>
#include <linux/errno.h>

struct device;
struct notifier_block;

/* The maximum number of frequency band ranges */
#define MAX_NUM_OF_WBRF_RANGES		11

/* Record actions */
#define WBRF_RECORD_ADD		0x0
#define WBRF_RECORD_REMOVE	0x1

struct freq_band_range {
	u64		start;
	u64		end;
};

struct wbrf_ranges_in_out {
	u64			num_of_ranges;
	struct freq_band_range	band_list[MAX_NUM_OF_WBRF_RANGES];
};

enum wbrf_notifier_actions {
	WBRF_CHANGED,
};

/* ---- CONFIG_AMD_WBRF = n: all ACPI WBRF paths are no-ops ---- */
static inline bool
acpi_amd_wbrf_supported_producer(struct device *dev)
{
	return false;
}
static inline int
acpi_amd_wbrf_add_remove(struct device *dev, uint8_t action,
			 struct wbrf_ranges_in_out *in)
{
	return -ENODEV;
}
static inline bool
acpi_amd_wbrf_supported_consumer(struct device *dev)
{
	return false;
}
static inline int
amd_wbrf_retrieve_freq_band(struct device *dev, struct wbrf_ranges_in_out *out)
{
	return -ENODEV;
}
static inline int
amd_wbrf_register_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline int
amd_wbrf_unregister_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}

/* legacy init/exit used by older call sites */
static inline void acpi_amd_wbrf_init(struct device *dev) { (void)dev; }
static inline void acpi_amd_wbrf_exit(struct device *dev) { (void)dev; }
static inline void acpi_amd_wbrf_enable(struct device *dev) { (void)dev; }
static inline void acpi_amd_wbrf_disable(struct device *dev) { (void)dev; }
static inline bool acpi_amd_wbrf_enabled(struct device *dev)
{
	(void)dev;
	return false;
}

#endif /* __LINUX_ACPI_AMD_WBRF_H */
