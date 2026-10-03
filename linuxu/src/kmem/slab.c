/* linuxu shim: slab — kmem_cache_* per-cache freelists
 * (REAL-minimal).  The header layout of
 * struct kmem_cache is owned by linuxu/headers/linux/slab.h; the runtime
 * freelist is a parallel side table keyed by cache pointer so the struct
 * layout stays header-defined.  (kmemalloc.c carries the kmalloc core;
 * this file only exists to keep the slab namespace + devm allocators in
 * one place for the build list.) */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/device.h>
#include <linux/i2c.h>

/* ---- devm allocations: simple per-device LIFO of (free) actions ---- */
struct devres_free_node {
	struct devres_free_node *next;
	struct devres_free_node *close_marker;
	void *ptr;
	void (*action)(void *data);
	void *data;
	void *group_id;
	unsigned char kind;
};

enum { DEVRES_RESOURCE, DEVRES_GROUP_OPEN, DEVRES_GROUP_CLOSE };

struct devres_list {
	struct devres_free_node *head;
};

/* List ownership is separate from callback execution. Callbacks run without
 * the list lock and may recursively release resources. Competing release
 * threads wait for the current device's callbacks before returning. */
static pthread_mutex_t devres_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t devres_idle = PTHREAD_COND_INITIALIZER;
struct devres_execution {
	struct devres_execution *next;
	struct device *dev;
	pthread_t owner;
	int all;
};
static struct devres_execution *devres_executions;

static int devres_enter_locked(struct device *dev,
			       struct devres_execution *execution, int all)
{
	for (;;) {
		struct devres_execution *active;
		for (active = devres_executions; active; active = active->next) {
			if (active->dev != dev) continue;
			if (!pthread_equal(active->owner, pthread_self())) break;
			if (all && active->all) return 0;
		}
		if (!active) break;
		pthread_cond_wait(&devres_idle, &devres_lock);
	}
	*execution = (struct devres_execution){
		.next = devres_executions, .dev = dev,
		.owner = pthread_self(), .all = all,
	};
	devres_executions = execution;
	return 1;
}

static void devres_leave(struct devres_execution *execution)
{
	pthread_mutex_lock(&devres_lock);
	struct devres_execution **link = &devres_executions;
	while (*link && *link != execution) link = &(*link)->next;
	if (*link) *link = execution->next;
	pthread_cond_broadcast(&devres_idle);
	pthread_mutex_unlock(&devres_lock);
}

static void *devm_ctx_locked(struct device *dev)
{
	if (!dev)
		return NULL;
	for (struct devres_execution *active = devres_executions; active; active = active->next)
		if (active->dev == dev && active->all) return NULL;
	struct devres_list **slot = (struct devres_list **)&dev->devres;
	struct devres_list *l = *slot;

	if (!l) {
		l = calloc(1, sizeof(*l));
		if (l)
			*slot = l;
	}
	return l;
}

void devres_init(struct device *dev)
{
	pthread_mutex_lock(&devres_lock);
	(void)devm_ctx_locked(dev);
	pthread_mutex_unlock(&devres_lock);
}

static int devres_run_nodes(struct devres_free_node *n)
{
	int released = 0;

	while (n) {
		struct devres_free_node *next = n->next;

		if (n->kind == DEVRES_RESOURCE && n->action)
			n->action(n->data);
		else if (n->kind == DEVRES_RESOURCE && n->ptr)
			kfree(n->ptr);
		if (n->kind == DEVRES_RESOURCE)
			released++;
		free(n->close_marker);
		free(n);
		n = next;
	}
	return released;
}

static int devres_add_node(struct devres_list *l, void *ptr,
			    void (*action)(void *data), void *data)
{
	struct devres_free_node *n = malloc(sizeof(*n));

	if (!n)
		return -ENOMEM;
	n->next = l->head;
	n->close_marker = NULL;
	n->ptr = ptr;
	n->action = action;
	n->data = data;
	n->group_id = NULL;
	n->kind = DEVRES_RESOURCE;
	l->head = n;
	return 0;
}

struct devres *devres_alloc(size_t size, gfp_t gfp)
{
	return kzalloc(size, gfp);
}

void devres_free(struct devres *res)
{
	kfree(res);
}

void devres_destroy(struct devres *res)
{
	kfree(res);
}

void devres_release(struct devres *res)
{
	kfree(res);
}

int devres_add(struct device *dev, struct devres *res)
{
	if (!dev || !res) return -EINVAL;
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = devm_ctx_locked(dev);
	int r = l ? devres_add_node(l, res, NULL, NULL) : -ENOMEM;
	pthread_mutex_unlock(&devres_lock);
	return r;
}

void devres_release_all(struct device *dev)
{
	struct devres_execution execution;
	if (!dev) return;
	pthread_mutex_lock(&devres_lock);
	if (!devres_enter_locked(dev, &execution, 1)) {
		pthread_mutex_unlock(&devres_lock);
		return;
	}
	struct devres_list *l = dev->devres;
	struct devres_free_node *head = l ? l->head : NULL;
	dev->devres = NULL;
	pthread_mutex_unlock(&devres_lock);
	free(l);
	devres_run_nodes(head);
	devres_leave(&execution);
}

/* Group markers bound the LIFO resources acquired by one probe/constructor.
 * Closed groups retain their markers so a later release excludes newer work. */
void *devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	(void)gfp;
	struct devres_free_node *n = calloc(1, sizeof(*n));
	if (!n) return NULL;
	n->close_marker = calloc(1, sizeof(*n));
	if (!n->close_marker) { free(n); return NULL; }
	n->kind = DEVRES_GROUP_OPEN;
	n->group_id = id ? id : n;
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = devm_ctx_locked(dev);
	if (!l) {
		pthread_mutex_unlock(&devres_lock);
		free(n->close_marker); free(n);
		return NULL;
	}
	n->next = l->head;
	l->head = n;
	void *group_id = n->group_id;
	pthread_mutex_unlock(&devres_lock);
	return group_id;
}

static struct devres_free_node *devres_find_group(struct devres_list *l,
						  void *id, int open_only)
{
	struct devres_free_node *n, *prior;

	for (n = l->head; n; n = n->next) {
		if (n->kind != DEVRES_GROUP_OPEN ||
		    (id && n->group_id != id))
			continue;
		if (!open_only)
			return n;
		for (prior = l->head; prior != n; prior = prior->next)
			if (prior->kind == DEVRES_GROUP_CLOSE &&
			    prior->group_id == n->group_id)
				break;
		if (prior == n)
			return n;
	}
	return NULL;
}

void devres_close_group(struct device *dev, void *id)
{
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = dev ? dev->devres : NULL;
	struct devres_free_node *open = l ? devres_find_group(l, id, 1) : NULL;
	if (open) {
		struct devres_free_node *close = open->close_marker;
		open->close_marker = NULL;
		close->kind = DEVRES_GROUP_CLOSE;
		close->group_id = open->group_id;
		close->next = l->head;
		l->head = close;
	}
	pthread_mutex_unlock(&devres_lock);
}

int devres_release_group(struct device *dev, void *id)
{
	struct devres_execution execution;
	if (!dev) return -ENOENT;
	pthread_mutex_lock(&devres_lock);
	(void)devres_enter_locked(dev, &execution, 0);
	struct devres_list *l = dev->devres;
	struct devres_free_node *open = l ? devres_find_group(l, id, 0) : NULL;
	if (!open) {
		pthread_mutex_unlock(&devres_lock);
		devres_leave(&execution);
		return -ENOENT;
	}
	struct devres_free_node *n, **start = &l->head;
	for (n = l->head; n != open; n = n->next) {
		if (n->kind == DEVRES_GROUP_CLOSE && n->group_id == open->group_id)
			break;
		start = &n->next;
	}
	if (n == open) start = &l->head;
	struct devres_free_node *first = *start;
	*start = open->next;
	open->next = NULL;
	pthread_mutex_unlock(&devres_lock);
	int released = devres_run_nodes(first);
	devres_leave(&execution);
	return released;
}

static void *devm_track_allocation(struct device *dev, void *p)
{
	if (ZERO_OR_NULL_PTR(p)) return p;
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = devm_ctx_locked(dev);
	int r = l ? devres_add_node(l, p, NULL, NULL) : -ENOMEM;
	pthread_mutex_unlock(&devres_lock);
	if (r) { kfree(p); return NULL; }
	return p;
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_track_allocation(dev, kmalloc(size, gfp));
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_kmalloc(dev, size, gfp | __GFP_ZERO);
}

void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return devm_kzalloc(dev, n * size, gfp);
}

void devm_kfree(struct device *dev, void *p)
{
	struct devres_free_node *found = NULL;
	if (ZERO_OR_NULL_PTR(p)) return;
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = dev ? dev->devres : NULL;
	if (l && p) {
		for (struct devres_free_node **link = &l->head; *link; link = &(*link)->next) {
			struct devres_free_node *n = *link;
			if (n->kind == DEVRES_RESOURCE && !n->action && n->ptr == p) {
				*link = n->next; found = n; break;
			}
		}
	}
	pthread_mutex_unlock(&devres_lock);
	if (found) { kfree(p); free(found); }
}

char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (len < 0)
		return NULL;
	p = kmalloc((size_t)len + 1, gfp);
	if (!p)
		return NULL;
	va_start(ap, fmt);
	vsnprintf(p, (size_t)len + 1, fmt, ap);
	va_end(ap);
	return devm_track_allocation(dev, p);
}

int devm_add_action(struct device *dev,
		     void (*cleanup_fn)(void *data), void *data)
{
	if (!cleanup_fn) return -EINVAL;
	pthread_mutex_lock(&devres_lock);
	struct devres_list *l = devm_ctx_locked(dev);
	int r = l ? devres_add_node(l, NULL, cleanup_fn, data) : -ENOMEM;
	pthread_mutex_unlock(&devres_lock);
	return r;
}

int devm_add_action_or_reset(struct device *dev,
			     void (*cleanup_fn)(void *data), void *data)
{
	if (!cleanup_fn) return -EINVAL;
	int r = devm_add_action(dev, cleanup_fn, data);
	if (r) cleanup_fn(data);
	return r;
}

void devm_release_action(struct device *dev, void (*release)(void *), void *data)
{
	struct devres_execution execution;
	struct devres_free_node *found = NULL;
	if (!dev || !release) return;
	pthread_mutex_lock(&devres_lock);
	(void)devres_enter_locked(dev, &execution, 0);
	struct devres_list *list = dev->devres;
	if (list) {
		for (struct devres_free_node **link = &list->head; *link; link = &(*link)->next) {
			struct devres_free_node *node = *link;
			if (node->kind == DEVRES_RESOURCE && node->action == release && node->data == data) {
				*link = node->next; found = node; break;
			}
		}
	}
	pthread_mutex_unlock(&devres_lock);
	if (found) { free(found); release(data); }
	devres_leave(&execution);
}

/* ---- unsupported managed IRQ / I/O mapping interfaces ---- */
int devm_request_irq(struct device *dev, unsigned int irq,
		     irq_handler_t handler, unsigned long flags,
		     const char *name, void *dev_id)
{
	(void)dev; (void)irq; (void)handler; (void)flags;
	(void)name; (void)dev_id;
	return -EOPNOTSUPP; /* No managed IRQ ownership record was installed. */
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq,
			      irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long flags, const char *name,
			      void *dev_id)
{
	(void)dev; (void)irq; (void)handler; (void)thread_fn;
	(void)flags; (void)name; (void)dev_id;
	return -EOPNOTSUPP; /* No managed IRQ ownership record was installed. */
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	(void)dev; (void)irq; (void)dev_id;
}

void *devm_ioremap_resource(struct device *dev, struct resource *res)
{
	(void)dev; (void)res;
	return NULL; /* TODO(linuxu): fake-MMIO token (pci/pdev_mmio.c) */
}

void *devm_ioremap(struct device *dev, phys_addr_t addr, size_t size)
{
	(void)dev; (void)addr; (void)size;
	return NULL; /* TODO(linuxu): fake-MMIO token */
}

struct devm_sysfs_group {
	struct device *dev;
	const struct attribute_group *group;
};
static void devm_sysfs_group_release(void *data)
{
	struct devm_sysfs_group *record = data;
	sysfs_remove_group(&record->dev->kobj, record->group);
	free(record);
}

int devm_device_add_group(struct device *dev,
			  const struct attribute_group *group)
{
	if (!dev || !group) return -EINVAL;
	struct devm_sysfs_group *record = malloc(sizeof(*record));
	if (!record) return -ENOMEM;
	*record = (struct devm_sysfs_group){ dev, group };
	struct devres_execution execution;
	pthread_mutex_lock(&devres_lock);
	(void)devres_enter_locked(dev, &execution, 0);
	pthread_mutex_unlock(&devres_lock);
	int result = sysfs_create_group(&dev->kobj, group);
	if (result) free(record);
	else result = devm_add_action_or_reset(dev, devm_sysfs_group_release, record);
	devres_leave(&execution);
	return result;
}

void devm_device_remove_group(struct device *dev,
			     const struct attribute_group *group)
{
	if (!dev || !group) return;
	struct devres_execution execution;
	struct devres_free_node *found = NULL;
	pthread_mutex_lock(&devres_lock);
	(void)devres_enter_locked(dev, &execution, 0);
	struct devres_list *list = dev->devres;
	if (list) for (struct devres_free_node **link = &list->head; *link; link = &(*link)->next) {
		struct devres_free_node *node = *link;
		if (node->kind == DEVRES_RESOURCE && node->action == devm_sysfs_group_release &&
		    ((struct devm_sysfs_group *)node->data)->group == group) {
			*link = node->next; found = node; break;
		}
	}
	pthread_mutex_unlock(&devres_lock);
	if (found) { devm_sysfs_group_release(found->data); free(found); }
	devres_leave(&execution);
}

/* devm_i2c_new_device: provided by shims/i2c.c (full i2c types needed).
 * The devm_i2c_delete_adapter/devm_i2c_del_adapter/
 * devm_i2c_add_numbered_adapter/devm_i2c_register_adapter definitions
 * also live in shims/i2c.c — the old local copies here caused
 * duplicate-symbol link failures. */

/* ---- kstrdup / kasprintf / kfree_const / krealloc / kmalloc_node_track_caller
 * (2026 drm core needs these; thin kmalloc wrappers) ---- */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *kstrdup(const char *s, unsigned int gfp)
{
	size_t n;
	char *p;

	if (!s)
		return NULL;
	n = strlen(s) + 1;
	p = (char *)kmalloc(n, gfp);
	if (p)
		memcpy(p, s, n);
	return p;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	char *p;
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (len < 0)
		return NULL;
	p = (char *)kmalloc(len + 1, gfp);
	if (!p)
		return NULL;
	va_start(ap, fmt);
	vsnprintf(p, len + 1, fmt, ap);
	va_end(ap);
	return p;
}

void kfree_const(const void *p)
{
	kfree((void *)p);
}

void *krealloc(const void *oldp, size_t size, gfp_t flags)
{
	void *np;
	size_t cpy;

	if (!size) {
		kfree(oldp);
		return ZERO_SIZE_PTR;
	}
	if (ZERO_OR_NULL_PTR(oldp))
		return kmalloc(size, flags);
	cpy = ksize(oldp);
	if (cpy > size)
		cpy = size;
	np = kmalloc(size, flags);
	if (!np)
		return NULL;
	if (cpy)
		memcpy(np, oldp, cpy);
	kfree(oldp);
	return np;
}

void *krealloc_array(const void *oldp, size_t n, size_t size, gfp_t flags)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return krealloc(oldp, n * size, flags);
}

void *kmalloc_node_track_caller(size_t size, gfp_t flags, int node)
{
	(void)node;
	return kmalloc(size, flags);
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	char *p;
	int len;
	va_list copy;

	va_copy(copy, ap);
	len = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (len < 0)
		return NULL;
	p = (char *)kmalloc(len + 1, gfp);
	if (!p)
		return NULL;
	va_copy(copy, ap);
	vsnprintf(p, len + 1, fmt, copy);
	va_end(copy);
	return p;
}

char *kstrdup_const(const char *s, unsigned int gfp)
{
	return kstrdup(s, gfp);
}
