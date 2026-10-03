/* linuxu shim: vram — VRAM allocator over a host buffer
 * (16 KB granularity, size from env
 * LINUXU_VRAM_MB default 16, lazy malloc).  pfn↔ptr mapping over the
 * buffer at 16 KB pages. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 16 KB page contract */
#define VRAM_PAGE_SHIFT 14
#define VRAM_PAGE_SIZE  (1UL << VRAM_PAGE_SHIFT)

static uint8_t *vram_buf;
static size_t vram_size;
static pthread_mutex_t vram_lock = PTHREAD_MUTEX_INITIALIZER;
static int vram_ready;

static void vram_init(void)
{
	size_t mb = 16;
	const char *env = getenv("LINUXU_VRAM_MB");

	if (env && *env) {
		char *end;
		errno = 0;
		unsigned long value = strtoul(env, &end, 10);
		if (errno || *end || *env == '-')
			return;
		mb = (size_t)value;
	}
	if (mb < 1)
		mb = 1;
	if (mb > SIZE_MAX / (1024 * 1024))
		return;
	size_t size = mb * 1024 * 1024;
	uint8_t *buffer = aligned_alloc(VRAM_PAGE_SIZE, size);
	if (buffer) {
		memset(buffer, 0, size);
		vram_buf = buffer;
		vram_size = size;
		vram_ready = 1;
	}
}

static void vram_ensure(void)
{
	if (!vram_ready)
		vram_init();
}

/* ---- pfn↔ptr ---- */
void *vram_pfn_to_ptr(uint32_t pfn)
{
	pthread_mutex_lock(&vram_lock);
	vram_ensure();
	void *p = vram_ready && pfn < vram_size / VRAM_PAGE_SIZE ?
		vram_buf + (size_t)pfn * VRAM_PAGE_SIZE : NULL;
	pthread_mutex_unlock(&vram_lock);
	return p;
}

uint32_t vram_ptr_to_pfn(const void *ptr)
{
	uintptr_t p = (uintptr_t)ptr;
	uint32_t pfn = UINT32_MAX;
	pthread_mutex_lock(&vram_lock);
	uintptr_t base = (uintptr_t)vram_buf;
	if (vram_ready && p >= base && p - base < vram_size &&
	    (p - base) / VRAM_PAGE_SIZE < UINT32_MAX)
		pfn = (uint32_t)((p - base) / VRAM_PAGE_SIZE);
	pthread_mutex_unlock(&vram_lock);
	return pfn;
}

/* ---- bump allocator (16KB granularity) ---- */
static uint64_t vram_bump;

void *linuxu_vram_alloc(size_t size)
{
	uint8_t *p;

	if (!size || size > SIZE_MAX - (VRAM_PAGE_SIZE - 1))
		return NULL;
	pthread_mutex_lock(&vram_lock);
	vram_ensure();
	size = (size + VRAM_PAGE_SIZE - 1) & ~(VRAM_PAGE_SIZE - 1);
	if (!vram_ready || size > vram_size - vram_bump) {
		pthread_mutex_unlock(&vram_lock);
		return NULL;
	}
	p = vram_buf + vram_bump;
	vram_bump += size;
	pthread_mutex_unlock(&vram_lock);
	return p;
}

void linuxu_vram_free(void *ptr)
{
	(void)ptr;
	/* TODO(linuxu): real free list; bump-only is fine for P0 tests
	 * that alloc-then-exit. */
}

size_t linuxu_vram_size(void)
{
	pthread_mutex_lock(&vram_lock);
	vram_ensure();
	size_t size = vram_size;
	pthread_mutex_unlock(&vram_lock);
	return size;
}

size_t linuxu_vram_used(void)
{
	pthread_mutex_lock(&vram_lock);
	size_t used = (size_t)vram_bump;
	pthread_mutex_unlock(&vram_lock);
	return used;
}
