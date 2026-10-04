/* Hop timing of Linux-file requests (rt/lx_timing.h). */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/device.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <rt/lx_timing.h>
#include <rt/task.h>

struct hop_total {
	uint64_t count, ns;
};
static struct hop_total timing[256][RT_LX_HOP_COUNT];

static const char *const hop_names[RT_LX_HOP_COUNT] = {
	"admit", "args", "frame", "pages", "copyin", "enter", "ioctl",
	"leave", "copyout", "release", "finish", "reply", "total", "spawn",
};

uint64_t rt_lx_time_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void rt_lx_timing_add(uint32_t cmd, enum rt_lx_hop hop, uint64_t ns)
{
	struct hop_total *t;

	if ((unsigned int)hop >= RT_LX_HOP_COUNT)
		return;
	t = &timing[cmd & 0xff][hop];
	__atomic_add_fetch(&t->count, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&t->ns, ns, __ATOMIC_RELAXED);
}

/* The runtime's primitives, timed where they run (the dext's DriverKit
 * implementations, or the host's): each line is the mean of a loop. */
#define PRIMITIVE_LOOPS 2000
static volatile uintptr_t primitive_sink;

static long show_primitives(char *buf, unsigned long size)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static DEFINE_SPINLOCK(spin);
	struct { const char *name; uint64_t ns; } r[8];
	unsigned int k = 0;
	uint64_t t;
	long len = 0;

#define TIME_LOOP(label, body) do { \
	t = rt_lx_time_ns(); \
	for (int i = 0; i < PRIMITIVE_LOOPS; i++) { body; } \
	r[k].name = label; r[k].ns = (rt_lx_time_ns() - t) / PRIMITIVE_LOOPS; k++; \
} while (0)
	TIME_LOOP("clock", primitive_sink += rt_lx_time_ns());
	TIME_LOOP("malloc_free_64", { void *p = malloc(64); primitive_sink += (uintptr_t)p; free(p); });
	TIME_LOOP("kzalloc_kfree_16k", { void *p = kzalloc(16384, GFP_KERNEL); primitive_sink += (uintptr_t)p; kfree(p); });
	TIME_LOOP("mutex", { pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex); });
	TIME_LOOP("spinlock", { spin_lock(&spin); spin_unlock(&spin); });
	TIME_LOOP("rcu_read", { rcu_read_lock(); rcu_read_unlock(); });
	TIME_LOOP("current", primitive_sink += (uintptr_t)linuxu_current_task_peek());
	TIME_LOOP("errno", primitive_sink += (uintptr_t)errno);
#undef TIME_LOOP
	for (unsigned int i = 0; i < k; i++) {
		int n = snprintf(buf + len, size - (unsigned long)len, "%s%s=%llu", i ? " " : "# primitives ns: ",
				 r[i].name, (unsigned long long)r[i].ns);

		if (n < 0 || (unsigned long)n >= size - (unsigned long)len)
			return len;
		len += n;
	}
	if ((unsigned long)len + 1 < size)
		buf[len++] = '\n';
	return len;
}

long rt_lx_timing_show(char *buf, unsigned long size)
{
	unsigned long len = 0;
	int n;

	len = (unsigned long)show_primitives(buf, size);
	n = snprintf(buf + len, size - len, "# nr count: hop=mean_ns ...\n");
	if (n < 0 || (unsigned long)n >= size - len)
		return (long)len;
	len += (unsigned long)n;
	for (unsigned int nr = 0; nr < 256; nr++) {
		uint64_t count = 0;

		for (unsigned int h = 0; h < RT_LX_HOP_COUNT; h++) {
			uint64_t c = __atomic_load_n(&timing[nr][h].count, __ATOMIC_RELAXED);

			if (c > count)
				count = c;
		}
		if (!count)
			continue;
		n = snprintf(buf + len, size - len, "0x%02x %llu:", nr, (unsigned long long)count);
		if (n < 0 || (unsigned long)n >= size - len)
			break;
		len += (unsigned long)n;
		for (unsigned int h = 0; h < RT_LX_HOP_COUNT; h++) {
			const uint64_t c = __atomic_load_n(&timing[nr][h].count, __ATOMIC_RELAXED);
			const uint64_t ns = __atomic_load_n(&timing[nr][h].ns, __ATOMIC_RELAXED);

			if (!c)
				continue;
			n = snprintf(buf + len, size - len, " %s=%llu", hop_names[h],
				     (unsigned long long)(ns / c));
			if (n < 0 || (unsigned long)n >= size - len)
				return (long)len;
			len += (unsigned long)n;
		}
		n = snprintf(buf + len, size - len, "\n");
		if (n < 0 || (unsigned long)n >= size - len)
			break;
		len += (unsigned long)n;
	}
	return (long)len;
}

static ssize_t mlg_lx_timing_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev;
	(void)attr;
	return rt_lx_timing_show(buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(mlg_lx_timing);

void rt_lx_timing_register(struct device *dev)
{
	/* -EEXIST once it is there: the first client of the device adds it. */
	if (dev)
		(void)device_create_file(dev, &dev_attr_mlg_lx_timing);
}
