/* linuxu: SHIM (third_party/linux/include/linux/slab.h)
 *
 * API surface of the slab allocator used by the KMD: kmalloc family,
 * kfree, kmem_cache_* and the size helpers. All non-trivial functions
 * are declared extern and implemented in linuxu/src/kmem/kmemalloc.c.
 * The *_obj / *_objs flexible-array variants
 * exist in recent kernels and are used heavily by the amdgpu tree.
 */
#ifndef _LINUX_SLAB_H
#define _LINUX_SLAB_H

#include <linux/types.h>
#include <linux/gfp.h>

/* Match Linux: an empty allocation succeeds without owning heap storage. */
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/*
 * slab_flags_t / gfp_t come from linux/types.h + linux/gfp.h.
 */

#define SLAB_HWCACHE_ALIGN	0U
#define SLAB_PANIC		0U
#define SLAB_NO_ACCOUNT		0U
#define SLAB_TYPESAFE_BY_RCU	0U

/*
 * struct kmem_cache - opaque handle to a slab cache.
 * The runtime (linuxu/src/kmem/) fills this in; driver code only ever
 * passes the pointer around, so a single leading tag field is enough
 * for the shim.
 */
struct kmem_cache_args {
	unsigned int align;
	unsigned int useroffset;
	unsigned int usersize;
};

struct kmem_cache {
	unsigned int      object_size;
	unsigned int      align;
	unsigned int      flags;
	const char        *name;
	/* runtime bookkeeping follows (owned by linuxu/src/kmem) */
};

/* ---- size helpers (static inline, as upstream) ---- */

#ifndef __round_up_pow2
#define __round_up_pow2(x, p)   (((x) + ((p) - 1)) & ~((p) - 1))
#endif

/* cleanup attribute shims (vendor 2026; real in host shim — see cleanup.h) */
#define cleanup(fn) __attribute__((cleanup(fn)))
#ifndef kfree_rcu_cleanup
static inline void kfree_rcu_cleanup(void *ptr)
{
	*(void **)ptr = NULL;
}
#endif

/*
 * KSIZE(n, align): size + alignment slack. Upstream:
 *   (sizeof(type) + align - 1) & ~(align - 1)  plus cacheline rounding.
 */
#define KSIZE(n, align) \
	((unsigned long)n ? __align_size(sizeof(n), (align)) : 0)

static inline size_t __align_size(size_t size, size_t align)
{
	return __round_up_pow2(size, align < sizeof(void *) ? sizeof(void *) : align);
}

#define KMALLOC_MIN_SIZE	(sizeof(void *))
#define KMALLOC_SHIFT_MIN	0
#define KMALLOC_SHIFT_HIGH	((8 * (int)sizeof(size_t)) - 4)

static inline size_t size_add(size_t a, size_t b)
{
	size_t result;
	return __builtin_add_overflow(a, b, &result) ? (size_t)-1 : result;
}
static inline size_t size_mul(size_t a, size_t b)
{
	size_t result;
	return __builtin_mul_overflow(a, b, &result) ? (size_t)-1 : result;
}

static inline bool size_add_overflows(size_t a, size_t b)
{
	size_t result;
	return __builtin_add_overflow(a, b, &result);
}
static inline bool size_mul_overflows(size_t a, size_t b)
{
	size_t result;
	return __builtin_mul_overflow(a, b, &result);
}

/*
 * kmalloc_caches: upstream indexes a 32-entry table by size class.
 * The shim runtime just needs a single default cache.
 */
struct kmem_cache;

/* ---- allocation API (runtime: linuxu/src/kmem/kmemalloc.c) ---- */

#include <linux/compiler.h>
#define ARCH_DMA_MINALIGN 64
#include <stdarg.h>
extern void *kmalloc(size_t size, gfp_t flags);
extern void *kzalloc(size_t size, gfp_t flags);
extern char *kasprintf(gfp_t gfp, const char *fmt, ...) __printf(2, 3);
extern char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap) __printf(2, 0);
extern char *kstrdup(const char *s, gfp_t gfp);
extern void *krealloc(const void *old, size_t new_size, gfp_t flags);
extern void *krealloc_array(const void *old, size_t n, size_t size, gfp_t flags);
extern void kfree_const(const void *p);
extern void *kmalloc_node(size_t size, gfp_t flags, int node);
extern void *kmalloc_node_track_caller(size_t size, gfp_t flags, int node);
extern void *kcalloc(size_t n, size_t size, gfp_t flags);
extern void *kmalloc_array(size_t n, size_t size, gfp_t flags);
static inline void *kmalloc_array_node(size_t n, size_t size, gfp_t flags, int node)
{
	(void)node; /* The shim exposes one memory node. */
	return kmalloc_array(n, size, flags);
}
extern void *kmemdup(const void *src, size_t size, gfp_t gfp);
extern void  kfree(const void *objp);
extern void  kvfree(const void *addr);
extern void  kfree_sensitive(const void *objp);
extern void  kvfree_sensitive(const void *addr, size_t len);
extern size_t ksize(const void *p);
extern char *kstrdup_const(const char *s, gfp_t flags);

/* flexible-array ("obj") variants used by recent kernels.
 * The 3-arg form (size, align, flags) is the shim runtime; the
 * 2-arg form (size, flags) the driver calls is a header alias to it
 * (align defaults to sizeof(long) at runtime). */
extern void *kmalloc_obj1(size_t size);
extern void *kzalloc_obj1(size_t size);
extern void *kvzalloc_obj1(size_t size);
extern void *kmalloc_objs1(size_t size);
extern void *kzalloc_objs1(size_t size);
extern void *kzalloc_flex1(size_t size);
extern void *kvzalloc_flex1(size_t size);

/* flexible-array ("obj"/"objs"/"flex") variants used by the pinned 2026
 * kernel. The driver mixes type and pointer forms:
 *   kzalloc_obj(TYPE) / kzalloc_obj(*PTR)
 *   kzalloc_objs(TYPE, N) / kzalloc_objs(*PTR, N)
 *   kmalloc_obj / kmalloc_objs, kvzalloc_obj / kvzalloc_objs
 *   kzalloc_flex(*PTR, FAM, N) / kvzalloc_flex(*PTR, FAM, N)
 * A two-token macro dispatch on whether the first token ends in '*':
 *   pointer form -> sizeof(*X)
 *   type form    -> sizeof(X)
 */
#define __linuxu_obj_is_ptr(TOK, _)  _##TOK
#define __linuxu_is_ptr(TOK, _)      TOK##__ptr
#define __linuxu_sz_type(X)          (sizeof(X))
#define __linuxu_sz_ptr(X)           (sizeof(*(X)))
#define __linuxu_sz1(TOK, X)  __linuxu_sz1i(TOK)
#define __linuxu_sz1i(TOK)    __linuxu_sz1x(TOK, TOK)
#define __linuxu_sz1x(TOK, X) __linuxu_sz1y(TOK)
#define __linuxu_sz1y(TOK)    __linuxu_sz1z(TOK##TOK)
#define __linuxu_sz1z(TOK)    (sizeof(TOK))
#define __linuxu_sz1z(TOK__ptr) (sizeof(*(TOK)))

/* The above token dance is not portable; use a simpler, robust helper.
 * For the pointer forms the driver writes kzalloc_obj(*p), so sizeof(*p)
 * is directly available. For the type forms it writes kzalloc_obj(TYPE),
 * so sizeof(TYPE) is available. We therefore define the macro to take the
 * element expression and apply sizeof, and rely on the fact that the type
 * names the driver uses are themselves valid sizeof arguments.  We split
 * into a _ptr variant (driver must call kzalloc_obj(*p)) and a default that
 * treats its single argument as a type. */
#define kmalloc_obj(X, ...)   kmalloc_obj1(sizeof(X))
#define kzalloc_obj(X, ...)   kzalloc_obj1(sizeof(X))
#define kvzalloc_obj(X, ...)  kvzalloc_obj1(sizeof(X))
#define kmalloc_objp(X)  kmalloc_obj1(sizeof(*(X)))
#define kzalloc_objp(X)  kzalloc_obj1(sizeof(*(X)))
#define kvzalloc_objp(X) kvzalloc_obj1(sizeof(*(X)))
#define kmalloc_objs(X, N, ...)   kmalloc_objs1(size_mul(sizeof(X), (N)))
#define kzalloc_objs(X, N, ...)   kzalloc_objs1(size_mul(sizeof(X), (N)))
#define kvzalloc_objs(X, N, ...)  kvzalloc_obj1(size_mul(sizeof(X), (N)))
/* flex-array sizing: X is a pointer to a complete struct with FAM as a
 * trailing flexible array member. Element size via (char*)FAM[1] - (char*)FAM[0]
 * is not valid with a single base. Instead: sizeof(*(X)) + sizeof(T)*N where
 * T is the element type. For a complete struct, ((S*)0)->FAM[0] has type T,
 * so sizeof(((S*)0)->FAM[0]) == sizeof(T). Use the type-based form. */
/* flex-array sizing. Driver forms:
 *   kzalloc_flex(*table, entries, N)  — *table is a value (struct)
 *   kzalloc_flex(struct foo, entries, N) — type name
 * Element type T of the trailing flex member FAM is recovered via
 * sizeof(((typeof(S*)0)->FAM[0])) where S is the struct type. */
#define __linuxu_flex_S(X)   (typeof(X) *)
#define __linuxu_flex_elem(X, FAM)  sizeof(((typeof(X) *)0)->FAM[0])
#define kzalloc_flex(X, FAM, N, ...)  \
	kzalloc_flex1(size_add(sizeof(X), size_mul(sizeof(((typeof(X) *)0)->FAM[0]), (N))))
#define kvzalloc_flex(X, FAM, N, ...) \
	kvzalloc_flex1(size_add(sizeof(X), size_mul(sizeof(((typeof(X) *)0)->FAM[0]), (N))))

/* kvmalloc_objs / kvmalloc_obj — driver uses these for array allocation */
#define kvmalloc_objs(X, N, ...)   kmalloc_objs1(size_mul(sizeof(X), (N)))
#define kvmalloc_obj(X, ...)       kmalloc_obj1(sizeof(X))
#define kvzalloc_objs(X, N, ...)   kvzalloc_flex1(size_mul(sizeof(X), (N)))

/* kvmalloc_array / kvzalloc_array (vendor 2026 vmalloc.h) */
extern void *kvmalloc_array(size_t n, size_t size, gfp_t flags);
extern void *kvzalloc_array(size_t n, size_t size, gfp_t flags);

extern struct kmem_cache *kmem_cache_create(const char *name, size_t size,
					    size_t align, unsigned long flags,
					    void (*ctor)(void *));
extern struct kmem_cache *kmem_cache_create_user(const char *name,
						 size_t size, size_t align,
						 unsigned long flags,
						 void (*ctor)(void *));
extern void kmem_cache_destroy(struct kmem_cache *cachep);
extern void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags);
extern void *kmem_cache_zalloc(struct kmem_cache *cachep, gfp_t flags);
extern void *kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags,
				   int node);
extern void *kmem_cache_alloc_node_flags(struct kmem_cache *cachep,
					 gfp_t flags, int node);
extern void kmem_cache_free(struct kmem_cache *cachep, void *objp);
extern int  kmem_cache_shrink(struct kmem_cache *cachep);

#define KMEM_CACHE(name, flags) kmem_cache_create(#name, sizeof(struct name), \
		__alignof__(struct name), flags, NULL)

/* ---- misc ---- */

extern int   slab_is_available(void);
extern bool  slab_nomem(void);
extern void  slab_wake_waiters(unsigned long unused);

#define SLAB_MAX_ORDER 6

static inline size_t kmalloc_size(size_t size, size_t align)
{
	return KSIZE(size, align);
}


#endif /* _LINUX_SLAB_H */
