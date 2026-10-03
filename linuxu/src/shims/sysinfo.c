/* Host RAM statistics in Linux page units; these are not PCI/DART windows. */
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/sysinfo.h>

#ifdef LINUXU_DEXT_DK
/* Public DriverKit/IOLib.h C ABI; kern_return_t is a signed int. */
extern int IOSysCtlByName(const char *, void *, size_t *, void *, size_t);
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t memory_bytes;
static uint32_t native_page_size;
static pg_data_t memory_node;

#if defined(LINUXU_DEXT_DK) || defined(__APPLE__)
static int query(const char *name, void *value, size_t length)
{
	size_t actual = length;
#ifdef LINUXU_DEXT_DK
	int error = IOSysCtlByName(name, value, &actual, NULL, 0);
#else
	int error = sysctlbyname(name, value, &actual, NULL, 0);
#endif
	return error || actual != length ? -EIO : 0;
}

static int read_capacity(uint64_t *bytes, uint32_t *page_size)
{
	int error = query("hw.memsize", bytes, sizeof(*bytes));
	return error ? error : query("hw.pagesize", page_size, sizeof(*page_size));
}

static int read_free_bytes(uint32_t page_size, uint64_t *bytes)
{
	uint32_t pages = 0;
	int error = query("vm.page_free_count", &pages, sizeof(pages));
	if (!error) *bytes = (uint64_t)pages * page_size;
	return error;
}
#else
static int read_capacity(uint64_t *bytes, uint32_t *page_size)
{
	long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
	if (pages <= 0 || size <= 0 || (unsigned long)size > UINT32_MAX ||
	    (uint64_t)pages > UINT64_MAX / (unsigned long)size) return -EIO;
	*bytes = (uint64_t)pages * (unsigned long)size;
	*page_size = (uint32_t)size;
	return 0;
}

static int read_free_bytes(uint32_t page_size, uint64_t *bytes)
{
	long pages = sysconf(_SC_AVPHYS_PAGES);
	if (pages < 0 || (uint64_t)pages > UINT64_MAX / page_size) return -EIO;
	*bytes = (uint64_t)pages * page_size;
	return 0;
}
#endif

int linuxu_sysinfo_init(void)
{
	uint64_t bytes = 0, free_bytes = 0;
	uint32_t page_size = 0;
	int error = 0;
	pthread_mutex_lock(&memory_lock);
	if (memory_bytes) goto done;
	error = read_capacity(&bytes, &page_size);
	if (error) goto done;
	if (bytes < PAGE_SIZE || !page_size || (page_size & (page_size - 1)) ||
	    (bytes >> PAGE_SHIFT) > LONG_MAX) {
		error = -EINVAL;
		goto done;
	}
	error = read_free_bytes(page_size, &free_bytes);
	if (error) goto done;
	if (free_bytes > bytes) { error = -EINVAL; goto done; }
	for (unsigned int i = 0; i < MAX_NR_ZONES; i++) {
		memory_node.node_zones[i].zone_id = i;
		memory_node.node_zones[i].zone_pgdat = &memory_node;
	}
	memory_node.node_zones[ZONE_NORMAL].managed_pages = bytes >> PAGE_SHIFT;
	memory_node.per_node_pages = bytes >> PAGE_SHIFT;
	native_page_size = page_size;
	memory_bytes = bytes;
done:
	pthread_mutex_unlock(&memory_lock);
	return error;
}

int si_meminfo(struct sysinfo *info)
{
	if (!info) return -EINVAL;
	memset(info, 0, sizeof(*info));
	int error = linuxu_sysinfo_init();
	if (error) return error;
	/* Capacity and topology are immutable after successful publication. */
	info->totalram = memory_bytes >> PAGE_SHIFT;
	info->mem_unit = PAGE_SIZE;
	uint64_t free_bytes = 0;
	error = read_free_bytes(native_page_size, &free_bytes);
	if (error) return error;
	if (free_bytes > memory_bytes) return -EINVAL;
	info->freeram = free_bytes >> PAGE_SHIFT;
	return 0;
}

pg_data_t *linuxu_node_data(int nid)
{
	if (nid != 0 || linuxu_sysinfo_init()) return NULL;
	return &memory_node;
}
