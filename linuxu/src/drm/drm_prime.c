/* linuxu shim: drm_prime — PRIME import/export stubs
 * (REDIRECT: dma-buf is a plain struct; in-process
 * sharing needs no export). */
#include <linux/dma-fence.h>

/* Note: struct dma_buf / dma_buf_attachment are fully defined by
 * <linux/dma-buf.h> (pinned vendor 2026 layout). */

int drm_prime_handle_to_fd(struct drm_device *dev,
			   struct drm_file *file_priv,
			   uint32_t handle, uint32_t flags,
			   uint32_t *out_fd)
{
	(void)dev; (void)file_priv; (void)handle; (void)flags;
	*out_fd = -1;
	return -38;
}
int drm_prime_fd_to_handle(struct drm_device *dev,
			   struct drm_file *file_priv,
			   int fd, uint32_t *out_handle)
{
	(void)dev; (void)file_priv; (void)fd; (void)out_handle;
	return -38;
}
