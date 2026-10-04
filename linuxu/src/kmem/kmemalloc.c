/* linuxu shim: kmemalloc — kmalloc/kfree family over libc malloc, with
 * kmemcheck canaries and tracking in debug builds (DEBUG) only.
 * Kernel arg order preserved (e.g. memcmp(dest, src, n)); kzalloc zeroes;
 * __GFP_ZERO is honored; node affinity is unused in this single process.
 *
 * Every nonzero allocation carries a leading header: the payload's size
 * (ksize) and the allocation to free (the payload is aligned inside it).
 * Debug builds add kmemcheck's fields and canaries around the payload (see
 * kmemcheck.c for the layout) and track every object, which costs a lock
 * per kmalloc and kfree; release builds have neither. */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/string.h>

#if DEBUG
/* shared with kmemcheck.c (same directory, same translation unit set) */
struct kmemcheck_hdr {
	uintptr_t user_ptr;
	size_t    size;
	size_t    alloc_size;
	uint64_t  magic;
	uint64_t  canary_lo_pat;
	void     *allocation;
	size_t    slot;
};
extern int kmemcheck_track(struct kmemcheck_hdr *hdr, size_t user_size);
extern int kmemcheck_untrack(struct kmemcheck_hdr *hdr);
extern int kmemcheck_scan(void);
#define KM_CANARY sizeof(uint64_t)	/* a canary word on each side */
#else
struct kmemcheck_hdr {
	uintptr_t user_ptr;
	size_t    size;
	size_t    alloc_size;
	void     *allocation;
};
#define KM_CANARY 0
#endif

/* The header of the payload at @user_ptr. */
static struct kmemcheck_hdr *km_hdr(const void *user_ptr)
{
	return (struct kmemcheck_hdr *)((uintptr_t)user_ptr - KM_CANARY -
					sizeof(struct kmemcheck_hdr));
}

#define KM_ALIGN 8u
static size_t km_align(size_t s)
{
	return (s + (KM_ALIGN - 1)) & ~(size_t)(KM_ALIGN - 1);
}

/* total bytes to malloc for a payload of user_size */
static size_t km_total(size_t user_size, size_t alignment)
{
	const size_t overhead = sizeof(struct kmemcheck_hdr) + 2 * KM_CANARY;
	if (!alignment || (alignment & (alignment - 1)) ||
	    alignment - 1 > SIZE_MAX - overhead ||
	    user_size > SIZE_MAX - overhead - (alignment - 1))
		return 0;
	return user_size + overhead + alignment - 1;
}

static void *km_alloc_aligned(size_t user_size, size_t alignment)
{
	size_t total = km_total(user_size, alignment);
	if (!total)
		return NULL;
	unsigned char *raw = malloc(total);
	struct kmemcheck_hdr *hdr;
	uintptr_t user;

	if (!raw)
		return NULL;
	user = ((uintptr_t)raw + sizeof(*hdr) + KM_CANARY + alignment - 1) &
		~(uintptr_t)(alignment - 1);
	hdr = km_hdr((void *)user);
	hdr->user_ptr = user;
	hdr->alloc_size = total;
	hdr->allocation = raw;
#if DEBUG
	kmemcheck_track(hdr, user_size);
#else
	hdr->size = user_size;
#endif
	return (void *)hdr->user_ptr;
}

static void *km_alloc(size_t user_size)
{
	return km_alloc_aligned(user_size, ARCH_DMA_MINALIGN);
}

static void km_dealloc(void *user_ptr)
{
	struct kmemcheck_hdr *hdr;

	if (ZERO_OR_NULL_PTR(user_ptr))
		return;
	hdr = km_hdr(user_ptr);
#if DEBUG
	kmemcheck_untrack(hdr);
#endif
	free(hdr->allocation);
}

/* ---- core API (linux/slab.h) ---- */
void *kmalloc(size_t size, gfp_t flags)
{
	if (size == 0)
		return ZERO_SIZE_PTR;
	void *p = km_alloc(size);
	if (p && (flags & __GFP_ZERO)) memset(p, 0, size);
	return p;
}

void *kzalloc(size_t size, gfp_t flags)
{
	return kmalloc(size, flags | __GFP_ZERO);
}

void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return kzalloc(n * size, flags);
}

void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return kmalloc(n * size, flags);
}

void *kmemdup(const void *src, size_t size, gfp_t gfp)
{
	void *p = kmalloc(size, gfp);

	if (p && src && size)
		memcpy(p, src, size);
	return p;
}

void kfree(const void *objp)
{
	km_dealloc((void *)objp);
}

void linuxu_kfree_rcu_callback(struct rcu_head *head)
{
	kfree(head->linuxu_free_pointer);
}

/* Volatile stores keep sensitive data clearing observable before free. */
static void km_clear_sensitive(const void *pointer, size_t size)
{
	volatile unsigned char *bytes = (volatile unsigned char *)pointer;
	while (size--)
		*bytes++ = 0;
}

void kfree_sensitive(const void *objp)
{
	if (ZERO_OR_NULL_PTR(objp))
		return;
	km_clear_sensitive(objp, ksize(objp));
	kfree(objp);
}

void kvfree_sensitive(const void *addr, size_t len)
{
	if (ZERO_OR_NULL_PTR(addr))
		return;
	km_clear_sensitive(addr, len);
	kvfree(addr);
}

/* kmemcheck: in debug builds the slab allocator (kmalloc/kfree) is
 * kmemcheck-instrumented (canary words + per-size checksum around every
 * object); release builds track nothing, and the checks find nothing. */
int kmemcheck_enabled(void)
{
#if DEBUG
	return 1;
#else
	return 0;
#endif
}

/* Scan all live allocations; returns number of corrupted objects. */
int kmemcheck_verify_all(void)
{
#if DEBUG
	return kmemcheck_scan();
#else
	return 0;
#endif
}

/* ---- flexible-array ("obj") single-arg runtimes ----
 * The linuxu slab.h header defines kmalloc_obj(X)/kzalloc_obj(X)/
 * kmalloc_objs(X,N)/kzalloc_flex(X,FAM,N)/kvmalloc_obj etc. as macros
 * over the single-arg *_obj1/*_objs1/*_flex1 runtimes (the size is
 * computed at the call site).  The old 3-arg *_obj3 runtimes here were
 * removed from the header, so their definitions are gone; only the
 * 1-arg runtimes below remain. */
void *kmalloc_obj1(size_t size)
{
	return kmalloc(size, 0);
}

void *kzalloc_obj1(size_t size)
{
	return kzalloc(size, 0);
}

void *kvzalloc_obj1(size_t size)
{
	return kzalloc(size, 0);
}

void *kmalloc_objs1(size_t size)
{
	return kmalloc(size, 0);
}

void *kzalloc_objs1(size_t size)
{
	return kzalloc(size, 0);
}

void *kzalloc_flex1(size_t size)
{
	return kzalloc(size, 0);
}

void *kvzalloc_flex1(size_t size)
{
	return kzalloc(size, 0);
}

/* ---- misc (kernel.h / slab.h) ---- */
void *kvmalloc(size_t size, gfp_t flags)
{
	return kmalloc(size, flags);
}

void *kvzalloc(size_t size, gfp_t flags)
{
	return kzalloc(size, flags);
}

void kvfree(const void *addr)
{
	kfree(addr);
}

/* kvasprintf: defined in kmem/slab.c (single definition point). */

/* ---- slab cache (simplified: per-cache freelist over kmalloc) ----
 * The header's struct kmem_cache is header-owned (slab.h); the freelist
 * lives in a side table keyed by cache pointer so the struct layout
 * stays header-defined. */
struct cache_side {
	struct kmem_cache *cache;
	void *freelist;          /* singly-linked via first pointer */
	int used;
};
#define CACHE_SIDE_SZ 256
static struct cache_side cache_side[CACHE_SIDE_SZ];
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds cache_lock. Reserve only at creation, so exhaustion fails
 * before objects can be handed out and free never creates bookkeeping. */
static struct cache_side *cache_side_for(struct kmem_cache *c, int reserve)
{
	int i, empty = -1;

	for (i = 0; i < CACHE_SIDE_SZ; i++) {
		if (cache_side[i].used && cache_side[i].cache == c)
			return &cache_side[i];
		if (!cache_side[i].used && empty < 0)
			empty = i;
	}
	if (reserve && empty >= 0) {
		cache_side[empty] = (struct cache_side){.cache = c, .used = 1};
		return &cache_side[empty];
	}
	return NULL;
}

static void *cache_alloc(struct kmem_cache *c, int zero)
{
	pthread_mutex_lock(&cache_lock);
	struct cache_side *cs = cache_side_for(c, 0);
	void *p;

	if (!cs) {
		pthread_mutex_unlock(&cache_lock);
		return NULL;
	}
	if (cs->freelist) {
		p = cs->freelist;
		cs->freelist = *(void **)p;
		pthread_mutex_unlock(&cache_lock);
	} else {
		pthread_mutex_unlock(&cache_lock);
		p = km_alloc_aligned(c->object_size, c->align);
		if (!p)
			return NULL;
	}
	if (zero)
		memset(p, 0, c->object_size);
	return p;
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
				     size_t align, unsigned long flags,
				     void (*ctor)(void *))
{
	struct kmem_cache *c;
	if (!size || size > UINT_MAX - (KM_ALIGN - 1) || align > UINT_MAX ||
	    (align && (align & (align - 1))))
		return NULL;
	c = calloc(1, sizeof(*c));
	unsigned int a = align < ARCH_DMA_MINALIGN ? ARCH_DMA_MINALIGN : (unsigned)align;

	(void)ctor;
	if (!c)
		return NULL;
	c->object_size = (unsigned)km_align(size);
	c->align = a;
	c->flags = (unsigned)flags;
	c->name = name;
	pthread_mutex_lock(&cache_lock);
	struct cache_side *cs = cache_side_for(c, 1);
	pthread_mutex_unlock(&cache_lock);
	if (!cs) {
		free(c);
		return NULL;
	}
	return c;
}

struct kmem_cache *kmem_cache_create_user(const char *name, size_t size,
					  size_t align, unsigned long flags,
					  void (*ctor)(void *))
{
	(void)ctor;
	return kmem_cache_create(name, size, align, flags, NULL);
}

void kmem_cache_destroy(struct kmem_cache *cachep)
{
	struct cache_side *cs;
	void *p;

	if (!cachep)
		return;
	pthread_mutex_lock(&cache_lock);
	cs = cache_side_for(cachep, 0);
	p = cs ? cs->freelist : NULL;
	if (cs)
		memset(cs, 0, sizeof(*cs));
	pthread_mutex_unlock(&cache_lock);
	while (p) {
		void *nxt = *(void **)p;

		km_dealloc(p);
		p = nxt;
	}
	free(cachep);
}

void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	(void)flags;
	return cachep ? cache_alloc(cachep, 0) : NULL;
}

void *kmem_cache_zalloc(struct kmem_cache *cachep, gfp_t flags)
{
	(void)flags;
	return cachep ? cache_alloc(cachep, 1) : NULL;
}

void *kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags, int node)
{
	(void)node;
	return kmem_cache_alloc(cachep, flags);
}

void *kmem_cache_alloc_node_flags(struct kmem_cache *cachep, gfp_t flags,
				  int node)
{
	(void)node;
	return kmem_cache_alloc(cachep, flags);
}

void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	struct cache_side *cs;

	if (!cachep || !objp)
		return;
	pthread_mutex_lock(&cache_lock);
	cs = cache_side_for(cachep, 0);
	if (cs) {
		*(void **)objp = cs->freelist;
		cs->freelist = objp;
	}
	pthread_mutex_unlock(&cache_lock);
	if (!cs)
		km_dealloc(objp);
}

int kmem_cache_shrink(struct kmem_cache *cachep)
{
	(void)cachep;
	return 0;
}

int slab_is_available(void)
{
	return 1;
}

bool slab_nomem(void)
{
	return false;
}

void slab_wake_waiters(unsigned long unused)
{
	(void)unused;
}

size_t ksize(const void *p)
{
	struct kmemcheck_hdr *hdr;

	if (ZERO_OR_NULL_PTR(p))
		return 0;
	hdr = km_hdr(p);
	return hdr->size;
}

/* kvmalloc array forms (vendor 2026 vmalloc.h; in-process: slab-backed) */
void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return kvmalloc(n * size, flags);
}

void *kvzalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return kvzalloc(n * size, flags);
}

/* User addresses belong to a different process. Use the same checked
 * access policy as the Linux copy helpers, including empty-copy semantics. */
#include <linux/uaccess.h>
#include <linux/err.h>
void *memdup_user(const void __user *src, size_t size)
{
	void *p;

	if (!access_ok(src, size))
		return ERR_PTR(-EFAULT);
	p = kmalloc(size, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, size)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}

void *vmemdup_user(const void __user *src, size_t size)
{
	return memdup_user(src, size);
}

void *memdup_user_nul(const void __user *src, size_t size)
{
	void *p;

	if (size == SIZE_MAX)
		return ERR_PTR(-ENOMEM);
	if (!access_ok(src, size))
		return ERR_PTR(-EFAULT);
	p = kmalloc(size + 1, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, size)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	((char *)p)[size] = '\0';
	return p;
}
