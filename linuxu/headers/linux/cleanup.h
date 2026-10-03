/* linuxu: SHIM (third_party/linux/include/linux/cleanup.h) — real
 * __cleanup/guard() machinery over GCC/Clang cleanup attributes.
 *
 * Upstream shapes kept verbatim (DEFINE_FREE, __free, DEFINE_CLASS,
 * DEFINE_GUARD, guard, scoped_guard, ACQUIRE, no_free_ptr, return_ptr,
 * CLASS). The `cleanup(fn)` keyword spelling used by the fork's driver
 * sources (amdgpu_device.c: `struct amdgpu_hive_info *hive
 * cleanup(xgmi_put_hive)`) is mapped onto the same attribute, and the
 * kfree/free trampolines the driver references are provided here. */
#ifndef _LINUX_CLEANUP_H
#define _LINUX_CLEANUP_H

#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/slab.h>

/* ---- unique label helper ---- */
#define linuxu_unique_id_raw(prefix, num) prefix##num
#define linuxu_unique_id(prefix, num) linuxu_unique_id_raw(prefix, num)
#define __UNIQUE_ID(prefix) linuxu_unique_id(prefix, __COUNTER__)

#define __no_context_analysis

/* ---- __cleanup / cleanup ---- */
/*
 * The fork's driver sources use the bare `cleanup(fn)` attribute spelling;
 * the macro lives in <linux/slab.h> (its canonical home) and must be in
 * scope at the point of use.
 */
#define __cleanup(fn) __attribute__((cleanup(fn)))


/*
 * DEFINE_FREE(name, type, free) — the single `__free_##name` helper takes a
 * `void *` (kfree semantics) so it can be bound via the cleanup attribute
 * to variables of the pointed-at type.
 */
#define DEFINE_FREE(name, type, free) \
	static inline void __free_##name(void *p) { \
		type _T = *(type *)p; \
		{ free; } \
		*(type *)p = 0; \
	}

/* __free(name) — upstream attribute binding */
#define __free(name) cleanup(__free_##name)

/* __free(kfree) trampolines — the driver uses __free(kfree) directly
 * without a local DEFINE_FREE; upstream defines them in slab.h. */
static inline void __free_kfree(void *p)
{
	void *_T = *(void **)p;
	{ if (_T) kfree(_T); }
	*(void **)p = 0;
}
static inline void kfree_cleanup(void **ptr)
{
	if (*ptr) {
		kfree(*ptr);
		*ptr = NULL;
	}
}

/* __free(put_device) — upstream slab.h helper. */
struct device;
extern void put_device(struct device *dev);
static inline void __free_put_device(void *p)
{
	struct device *_T = *(struct device **)p;
	{ if (_T) put_device(_T); }
	*(struct device **)p = 0;
}

/* DEFINE_FREE(drm_bridge_put, ...) in drm_bridge.h expands to two static
 * inlines per TU; declare the underlying function first. */
struct drm_bridge;
extern void drm_bridge_put(struct drm_bridge *bridge);

/* ---- no_free_ptr / return_ptr ---- */
#define __get_and_null(p, nullvalue) \
	({ \
		__typeof__(&(p)) __ptr = &(p); \
		__typeof__(*__ptr) __val = *__ptr; \
		*__ptr = nullvalue; \
		__val; \
	})

static __always_inline __must_check
const volatile void * __must_check_fn(const volatile void *val)
{ return val; }

/* Preserve the pointer type and evaluate the lvalue's address once. */
#define no_free_ptr(p) \
	((__typeof__(p))__must_check_fn(__get_and_null(p, NULL)))

#define return_ptr(p)	return no_free_ptr(p)

#define retain_and_null_ptr(p)		((void)__get_and_null(p, NULL))

/* ---- classes (DEFINE_CLASS / CLASS / scoped_class) ---- */
#define DEFINE_CLASS(name, type, exit, init, init_args...) \
typedef type class_##name##_t; \
typedef type lock_##name##_t; \
static __always_inline void class_##name##_destructor(type *p) \
	__no_context_analysis \
{ type _T = *p; { exit; } (void)_T; } \
static __always_inline type class_##name##_constructor(init_args) \
	__no_context_analysis \
{ type t = init; return t; }

#define CLASS(name, var) \
	class_##name##_t var cleanup(class_##name##_destructor) = \
		class_##name##_constructor

#define CLASS_INIT(_name, _var, _init_expr) \
	class_##_name##_t _var cleanup(class_##_name##_destructor) = (_init_expr)

#define __scoped_class(_name, var, _label, args...) \
	for (CLASS(_name, var)(args); ; ({ goto _label; })) \
		if (0) { \
_label: \
			break; \
		} else

#define scoped_class(_name, var, args...) \
	__scoped_class(_name, var, __UNIQUE_ID(label), args)

/* ---- guards (locks) ---- */
#define DEFINE_GUARD(name, type, lock, unlock) \
	DEFINE_CLASS(name, type, { if (_T) { unlock; } }, ({ lock; _T; }), type _T);

/* A guard holds exactly the class value; cleanup receives its address. */
#define guard(name) CLASS(name, __UNIQUE_ID(guard))
#define scoped_guard(_name, args...) \
	__scoped_class(_name, scope, __UNIQUE_ID(label), args)

/* ACQUIRE(name, var) / ACQUIRE_ERR */
#define ACQUIRE(_name, _var) CLASS(_name, _var)
#define ACQUIRE_ERR(_name, _var) (0)

DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))

#endif /* _LINUX_CLEANUP_H */
