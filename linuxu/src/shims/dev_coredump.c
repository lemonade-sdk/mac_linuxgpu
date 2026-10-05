/* linuxu shim: dev_coredump — no exporter (devm_devcoredump_setup no-op);
 * dev_coredumpm logs the dump's head, which names the device, the ring that
 * hung and its process, at error level. */
#include <pthread.h>
#include <linux/device.h>
#include <linux/devcoredump.h>
#include <linux/printk.h>
#include <linux/string.h>

/* The dump's first bytes: amdgpu's header, time, process, ring and IP
 * state summary come first. */
#define COREDUMP_HEAD_BYTES	3072
#define COREDUMP_HEAD_LINES	48

void dev_coredumpm(struct device *dev, struct module *owner,
		   void *data, size_t datalen, gfp_t gfp,
		   ssize_t (*read)(char *buffer, loff_t offset, size_t count,
				   void *data, size_t datalen),
		   void (*free)(void *data))
{
	/* Rare (a GPU hang), and on the reset domain's single worker: one
	 * static buffer, so a dump never needs memory it may not get. */
	static pthread_mutex_t head_lock = PTHREAD_MUTEX_INITIALIZER;
	static char head[COREDUMP_HEAD_BYTES + 1];
	ssize_t n = 0;

	(void)owner;
	pthread_mutex_lock(&head_lock);
	if (dev && read)
		n = read(head, 0, COREDUMP_HEAD_BYTES, data, datalen);
	(void)gfp;
	if (dev)
		dev_err(dev, "devcoredump: the device dumped its state (head follows)\n");
	if (n > 0) {
		char *line = head;
		int lines = 0;

		head[n] = '\0';
		while (line && *line && lines++ < COREDUMP_HEAD_LINES) {
			char *end = strchr(line, '\n');

			if (end)
				*end = '\0';
			if (*line)
				dev_err(dev, "devcoredump: %s\n", line);
			line = end ? end + 1 : NULL;
		}
	}
	pthread_mutex_unlock(&head_lock);
	if (free)
		free(data);
}

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
