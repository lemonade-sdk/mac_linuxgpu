/* linuxu shim: dev_coredump — coredump no-op
 * (ENOSYS/NOOP: devm_devcoredump_setup no-op). */
#include <linux/device.h>

struct dev_coredump_mmap;

struct dev_coredump_mmap *devm_devcoredump_alloc(struct device *dev,
						 size_t size, gfp_t gfp,
						 int node)
{
	(void)dev; (void)size; (void)gfp; (void)node;
	return NULL;
}

int devm_devcoredump_setup(struct device *dev,
			   struct dev_coredump_mmap *m,
			   size_t size, gfp_t gfp, int node)
{
	(void)dev; (void)m; (void)size; (void)gfp; (void)node;
	return 0;
}

void devcoredump_free(struct dev_coredump_mmap *m)
{
	(void)m;
}
