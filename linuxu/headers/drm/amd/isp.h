/* linuxu: AS-IS (third_party/linux/include/drm/amd/isp.h) */
#ifndef __ISP_H__
#define __ISP_H__

#include <linux/types.h>

struct device;

struct isp_platform_data {
	void *adev;
	u32 asic_type;
	resource_size_t base_rmmio_size;
};

int isp_user_buffer_alloc(struct device *dev, void *dmabuf,
			  void **buf_obj, u64 *buf_addr);

void isp_user_buffer_free(void *buf_obj);

int isp_kernel_buffer_alloc(struct device *dev, u64 size,
			    void **buf_obj, u64 *gpu_addr, void **cpu_addr);

void isp_kernel_buffer_free(void **buf_obj, u64 *gpu_addr, void **cpu_addr);

#endif
