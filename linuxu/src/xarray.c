/* Sparse radix xarrays. Tree access is serialized independently of the
 * caller-visible xa_lock, so reads inside a caller critical section work. */
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <linux/xarray.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/rcupdate.h>

static pthread_mutex_t xa_tree_lock = PTHREAD_MUTEX_INITIALIZER;
#define XA_BITS (sizeof(unsigned long) * CHAR_BIT)

static bool xa_covers(const struct xa_node *node, unsigned long index)
{
	return node && (node->shift >= XA_BITS - 6 ||
		!(index >> (node->shift + 6)));
}
static struct xa_node *xa_node_new(unsigned int shift)
{
	struct xa_node *node = calloc(1, sizeof(*node));
	if (node) node->shift = shift;
	return node;
}
static void xa_free_subtree(struct xa_node *node)
{
	if (!node) return;
	if (node->shift)
		for (unsigned int i = 0; i < 64; i++)
			xa_free_subtree(node->entries[i]);
	free(node);
}
static void xa_free_node_rcu(struct rcu_head *head)
{
	free(container_of(head, struct xa_node, rcu));
}
static void xa_free_tree_rcu(struct rcu_head *head)
{
	xa_free_subtree(container_of(head, struct xa_node, rcu));
}

static struct xa_node *xa_path_new(unsigned int shift, unsigned long index,
				   void *entry)
{
	struct xa_node *node = xa_node_new(shift);
	if (!node) return NULL;
	unsigned int slot = (index >> shift) & 63;
	if (shift) {
		node->entries[slot] = xa_path_new(shift - 6, index, entry);
		if (!node->entries[slot]) { free(node); return NULL; }
	} else {
		node->entries[slot] = entry;
	}
	return node;
}
static struct xa_node *xa_leaf(struct xarray *xa, unsigned long index)
{
	struct xa_node *node = xa->xa_head;
	if (!xa_covers(node, index)) return NULL;
	while (node && node->shift)
		node = node->entries[(index >> node->shift) & 63];
	return node;
}
static void *xa_load_locked(struct xarray *xa, unsigned long index)
{
	struct xa_node *node = xa_leaf(xa, index);
	return node ? node->entries[index & 63] : NULL;
}
static bool xa_remove_locked(struct xa_node *node, unsigned long index)
{
	unsigned int slot = (index >> node->shift) & 63;
	if (node->shift) {
		struct xa_node *child = node->entries[slot];
		if (child && xa_remove_locked(child, index)) {
			node->entries[slot] = NULL;
			call_rcu(&child->rcu, xa_free_node_rcu);
		}
	} else {
		__atomic_store_n(&node->entries[slot], NULL, __ATOMIC_RELEASE);
		for (unsigned int mark = 0; mark < XA_MAX_MARKS; mark++)
			node->marks[mark] &= ~(1UL << slot);
	}
	for (unsigned int i = 0; i < 64; i++)
		if (node->entries[i]) return false;
	return true;
}
static int xa_store_locked(struct xarray *xa, unsigned long index,
			   void *entry, gfp_t gfp)
{
	(void)gfp;
	struct xa_node *original = xa->xa_head, *root = original, *node;
	if (!entry) {
		if (xa_covers(root, index) && xa_remove_locked(root, index)) {
			__atomic_store_n(&xa->xa_head, NULL, __ATOMIC_RELEASE);
			call_rcu(&root->rcu, xa_free_node_rcu);
		}
		return 0;
	}
	unsigned int shift = index ? ((XA_BITS - 1 - __builtin_clzl(index)) / 6) * 6 : 0;
	if (!root) {
		root = xa_path_new(shift, index, entry);
		if (!root) return -ENOMEM;
		__atomic_store_n(&xa->xa_head, root, __ATOMIC_RELEASE);
		return 0;
	}
	/* Grow every radix level. Keep the old root published until the whole
	 * missing path exists; OOM must not alter entries or alias indices. */
	while (root->shift < shift) {
		struct xa_node *above = xa_node_new(root->shift + 6);
		if (!above) goto failed;
		above->entries[0] = root;
		root = above;
	}
	node = root;
	while (node->shift) {
		unsigned int slot = (index >> node->shift) & 63;
		if (!node->entries[slot]) {
			struct xa_node *path = xa_path_new(node->shift - 6, index, entry);
			if (!path) goto failed;
			node->entries[slot] = path;
			__atomic_store_n(&xa->xa_head, root, __ATOMIC_RELEASE);
			return 0;
		}
		node = node->entries[slot];
	}
	__atomic_store_n(&node->entries[index & 63], entry, __ATOMIC_RELEASE);
	__atomic_store_n(&xa->xa_head, root, __ATOMIC_RELEASE);
	return 0;
failed:
	while (root != original) {
		struct xa_node *below = root->entries[0];
		free(root);
		root = below;
	}
	return -ENOMEM;
}

static void *xa_visible(void *entry)
{
	return entry == (void *)XA_ZERO_ENTRY ? NULL : entry;
}
int xa_err(const void *entry) { return IS_ERR(entry) ? (int)PTR_ERR(entry) : 0; }
bool xa_is_err(const void *entry) { return IS_ERR(entry); }
void *xa_load(struct xarray *xa, unsigned long index)
{
	if (!xa) return NULL;
	pthread_mutex_lock(&xa_tree_lock);
	void *entry = xa_load_locked(xa, index);
	pthread_mutex_unlock(&xa_tree_lock);
	return xa_visible(entry);
}
void *__xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	if (!xa || IS_ERR(entry)) return ERR_PTR(-EINVAL);
	pthread_mutex_lock(&xa_tree_lock);
	void *old = xa_load_locked(xa, index);
	int error = xa_store_locked(xa, index, entry, gfp);
	pthread_mutex_unlock(&xa_tree_lock);
	return error ? ERR_PTR(error) : xa_visible(old);
}
void *__xa_erase(struct xarray *xa, unsigned long index)
{
	return __xa_store(xa, index, NULL, 0);
}
void *xa_erase(struct xarray *xa, unsigned long index)
{
	if (!xa) return NULL;
	xa_lock(xa);
	void *old = __xa_erase(xa, index);
	xa_unlock(xa);
	return old;
}
int xa_store_range(struct xarray *xa, unsigned long index, unsigned long max,
		   void *entry, gfp_t gfp)
{
	if (index > max) return -ERANGE;
	int error;
	xa_lock(xa);
	do {
		error = xa_err(__xa_store(xa, index, entry, gfp));
		if (error || index == max) break;
		index++;
	} while (1);
	xa_unlock(xa);
	return error;
}
static bool xa_mark_test_locked(struct xarray *xa, unsigned long index,
				unsigned int mark)
{
	struct xa_node *node = xa_leaf(xa, index);
	return node && mark < XA_MAX_MARKS &&
		(node->marks[mark] & (1UL << (index & 63)));
}
int xa_set_mark(struct xarray *xa, unsigned long index, unsigned long mark, gfp_t gfp)
{
	(void)gfp;
	if (!xa || mark >= XA_MAX_MARKS) return -EINVAL;
	pthread_mutex_lock(&xa_tree_lock);
	struct xa_node *node = xa_leaf(xa, index);
	if (node && node->entries[index & 63]) node->marks[mark] |= 1UL << (index & 63);
	pthread_mutex_unlock(&xa_tree_lock);
	return 0;
}
void xa_clear_mark(struct xarray *xa, unsigned long index, unsigned long mark)
{
	if (!xa || mark >= XA_MAX_MARKS) return;
	pthread_mutex_lock(&xa_tree_lock);
	struct xa_node *node = xa_leaf(xa, index);
	if (node) node->marks[mark] &= ~(1UL << (index & 63));
	pthread_mutex_unlock(&xa_tree_lock);
}
bool xa_test_mark(struct xarray *xa, unsigned long index, unsigned long mark)
{
	if (!xa || mark >= XA_MAX_MARKS) return false;
	pthread_mutex_lock(&xa_tree_lock);
	bool result = xa_mark_test_locked(xa, index, mark);
	pthread_mutex_unlock(&xa_tree_lock);
	return result;
}
int xa_store_marked(struct xarray *xa, unsigned long index, void *entry,
		    unsigned long mark, gfp_t gfp)
{
	int error = xa_err(xa_store(xa, index, entry, gfp));
	return error ? error : xa_set_mark(xa, index, mark, gfp);
}
int xa_set_mark_range(struct xarray *xa, unsigned long index, unsigned long max,
		     unsigned long mark, gfp_t gfp)
{
	if (index > max) return -ERANGE;
	do {
		int error = xa_set_mark(xa, index, mark, gfp);
		if (error || index == max) return error;
		index++;
	} while (1);
}
void xa_clear_mark_range(struct xarray *xa, unsigned long index, unsigned long max,
			unsigned long mark)
{
	if (index > max) return;
	do { xa_clear_mark(xa, index, mark); } while (index != max && ++index);
}

/* Traverse occupied radix paths, never scan an unbounded numeric address space. */
static void *xa_find_node(struct xa_node *node, unsigned long prefix,
			 unsigned long start, unsigned long max,
			 unsigned long *found_index, int mark)
{
	if (!node) return NULL;
	for (unsigned int slot = 0; slot < 64; slot++) {
		void *entry = node->entries[slot];
		if (!entry || slot > (ULONG_MAX >> node->shift)) continue;
		unsigned long base = prefix | ((unsigned long)slot << node->shift);
		unsigned long end = base | (node->shift ? (1UL << node->shift) - 1 : 0);
		if (base > max) break;
		if (end < start) continue;
		if (node->shift) {
			entry = xa_find_node(entry, base, start, max, found_index, mark);
			if (entry) return entry;
		} else if (xa_visible(entry) &&
			   (mark < 0 || (node->marks[mark] & (1UL << slot)))) {
			*found_index = base;
			return entry;
		}
	}
	return NULL;
}
void *xa_find(struct xarray *xa, unsigned long *indexp, unsigned long max,
	      unsigned int action)
{
	(void)action;
	if (!xa || !indexp || *indexp > max) return NULL;
	pthread_mutex_lock(&xa_tree_lock);
	void *entry = xa_find_node(xa->xa_head, 0, *indexp, max, indexp, -1);
	pthread_mutex_unlock(&xa_tree_lock);
	return entry;
}
void *xa_find_after(struct xarray *xa, unsigned long *indexp, unsigned long max,
		   unsigned int action)
{
	if (!indexp || *indexp >= max) return NULL;
	(*indexp)++;
	return xa_find(xa, indexp, max, action);
}
void *xa_find_marked(struct xarray *xa, unsigned long *indexp, unsigned long max,
		     unsigned int action, unsigned int mark)
{
	(void)action;
	if (!xa || !indexp || *indexp > max || mark >= XA_MAX_MARKS) return NULL;
	pthread_mutex_lock(&xa_tree_lock);
	void *entry = xa_find_node(xa->xa_head, 0, *indexp, max, indexp, mark);
	pthread_mutex_unlock(&xa_tree_lock);
	return entry;
}
unsigned long xa_count(struct xarray *xa, unsigned long index, unsigned long max)
{
	unsigned long count = 0;
	while (xa_find(xa, &index, max, 0)) {
		count++;
		if (index == max) break;
		index++;
	}
	return count;
}
static int xa_alloc_locked(struct xarray *xa, unsigned long *indexp, void *entry,
			   unsigned long start, unsigned long max, gfp_t gfp)
{
	/* An XA_FLAGS_ALLOC1 array never hands out index 0 (Linux keeps it
	 * busy from xa_init_flags): DRM syncobj handles, among others, are
	 * nonzero, and 0 means "none" to their users. */
	if ((xa->xa_flags & XA_FLAGS_ALLOC1) && start == 0) start = 1;
	if (start > max) return -ENOSPC;
	unsigned long index = start;
	while (xa_load_locked(xa, index)) {
		if (index == max) return -ENOSPC;
		index++;
	}
	int result = xa_store_locked(xa, index, entry ? entry : (void *)XA_ZERO_ENTRY, gfp);
	if (!result) *indexp = index;
	return result;
}
int xa_alloc_max(struct xarray *xa, unsigned long *indexp, void *entry,
		 unsigned long max, gfp_t gfp)
{
	if (!xa || !indexp || IS_ERR(entry)) return -EINVAL;
	xa_lock(xa);
	pthread_mutex_lock(&xa_tree_lock);
	int result = xa_alloc_locked(xa, indexp, entry, *indexp, max, gfp);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(xa);
	return result;
}
int xa_alloc_limit(struct xarray *xa, u32 *indexp, void *entry,
		   struct xa_limit limit, gfp_t gfp)
{
	if (!xa || !indexp || limit.min > limit.max) return -EINVAL;
	unsigned long index = limit.min;
	int result = xa_alloc_max(xa, &index, entry, limit.max, gfp);
	if (!result) *indexp = (u32)index;
	return result == -ENOSPC ? -EBUSY : result;
}
static int xa_cyclic_locked(struct xarray *xa, unsigned long *indexp, void *entry,
			    unsigned long min, unsigned long max,
			    unsigned long *hintp, gfp_t gfp)
{
	unsigned long start = hintp && *hintp >= min && *hintp <= max ? *hintp : min;
	int result = xa_alloc_locked(xa, indexp, entry, start, max, gfp);
	if (result == -ENOSPC && start > min)
		result = xa_alloc_locked(xa, indexp, entry, min, start - 1, gfp);
	if (!result && hintp) *hintp = *indexp == max ? min : *indexp + 1;
	return result;
}
int xa_alloc_cyclic(struct xarray *xa, unsigned long *indexp, void *entry,
		    unsigned long max, unsigned long *hintp, gfp_t gfp)
{
	if (!xa || !indexp || IS_ERR(entry)) return -EINVAL;
	xa_lock(xa);
	pthread_mutex_lock(&xa_tree_lock);
	int result = xa_cyclic_locked(xa, indexp, entry, 0, max, hintp, gfp);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(xa);
	return result;
}
int xa_alloc_cyclic_limit(struct xarray *xa, u32 *indexp, void *entry,
			 struct xa_limit limit, u32 *next, gfp_t gfp)
{
	if (!xa || !indexp || !next || limit.min > limit.max || IS_ERR(entry)) return -EINVAL;
	unsigned long index = 0, hint = *next;
	xa_lock(xa);
	pthread_mutex_lock(&xa_tree_lock);
	int result = xa_cyclic_locked(xa, &index, entry, limit.min, limit.max, &hint, gfp);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(xa);
	if (!result) { *indexp = (u32)index; *next = (u32)hint; }
	return result == -ENOSPC ? -EBUSY : result;
}
int xa_alloc_cyclic_max(struct xarray *xa, unsigned long *indexp, void *entry,
		       unsigned long max, unsigned long *hintp, gfp_t gfp)
{ return xa_alloc_cyclic(xa, indexp, entry, max, hintp, gfp); }
int xa_alloc_cycle(struct xarray *xa, unsigned long *indexp, void *entry,
		   unsigned long max, gfp_t gfp)
{ unsigned long hint = *indexp; return xa_alloc_cyclic(xa, indexp, entry, max, &hint, gfp); }
int xa_reserve(struct xarray *xa, unsigned long index, gfp_t gfp)
{
	if (!xa) return -EINVAL;
	xa_lock(xa);
	pthread_mutex_lock(&xa_tree_lock);
	int error = xa_load_locked(xa, index) ? 0 : xa_store_locked(xa, index, (void *)XA_ZERO_ENTRY, gfp);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(xa);
	return error;
}
int xa_resv_set_mark(struct xarray *xa, unsigned long index, unsigned long mark, gfp_t gfp)
{ int error = xa_reserve(xa, index, gfp); return error ? error : xa_set_mark(xa, index, mark, gfp); }
int xa_reserve_alloc(struct xarray *xa, unsigned long *indexp, void *entry, unsigned long max, gfp_t gfp)
{ return xa_alloc_max(xa, indexp, entry, max, gfp); }
int xa_destroy(struct xarray *xa)
{
	if (!xa) return 0;
	xa_lock(xa);
	pthread_mutex_lock(&xa_tree_lock);
	struct xa_node *root = xa->xa_head;
	__atomic_store_n(&xa->xa_head, NULL, __ATOMIC_RELEASE);
	if (root) call_rcu(&root->rcu, xa_free_tree_rcu);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(xa);
	return 0;
}
/* ---- xas iterator subset ----
 *
 * XA_STATE stores the owning xarray in xs_parent. A current-array
 * fallback remains for legacy callers that explicitly select it. */
/* TLS fallback for the driverKit platform (no TLS) — process-global under
 * the serial one-queue bringup. See rcu.c for the LINUXU_DEXT_DK rationale. */
#ifdef LINUXU_DEXT_DK
#define LINUXU_TLS
#else
#define LINUXU_TLS __thread
#endif
static LINUXU_TLS struct xarray *xa_state_current;

struct xarray *xa_state_set_current(struct xarray *xa)
{
	xa_state_current = xa;
	return xa;
}

static struct xarray *xa_state_resolve(struct xa_state *xas)
{
	return (struct xarray *)xas->xs_parent ?
		(struct xarray *)xas->xs_parent : xa_state_current;
}

int xas_reset(struct xa_state *xas, unsigned long index)
{
	xas->xa_index = index;
	xas->xa_entry = NULL;
	return 0;
}

void *xas_load(struct xa_state *xas)
{
	struct xarray *xa = xa_state_resolve(xas);
	void *e;

	pthread_mutex_lock(&xa_tree_lock);
	e = xa ? xa_load_locked(xa, xas->xa_index) : NULL;
	pthread_mutex_unlock(&xa_tree_lock);
	return e;
}

int xas_set(struct xa_state *xas, unsigned long index)
{
	xas->xa_index = index;
	return 0;
}

int xas_store(struct xa_state *xas, void *entry)
{
	struct xarray *xa = xa_state_resolve(xas);

	if (!xa)
		return -EINVAL;
	return xa_err(__xa_store(xa, xas->xa_index, entry, 0));
}

void *xas_erase(struct xa_state *xas)
{
	struct xarray *xa = xa_state_resolve(xas);
	void *old;

	if (!xa)
		return NULL;
	old = __xa_erase(xa, xas->xa_index);
	xas->xa_entry = old;
	return old;
}

void *xas_find(struct xa_state *xas, unsigned long max)
{
	struct xarray *xa = xa_state_resolve(xas);
	xas->xa_entry = xa ? xa_find(xa, &xas->xa_index, max, 0) : NULL;
	return xas->xa_entry;
}
void *xas_next(struct xa_state *xas)
{
	if (xas->xa_index == ULONG_MAX) return NULL;
	xas->xa_index++;
	return xas_find(xas, ULONG_MAX);
}
void *xas_find_marked(struct xa_state *xas, unsigned long max, unsigned int mark)
{
	struct xarray *xa = xa_state_resolve(xas);
	xas->xa_entry = xa ? xa_find_marked(xa, &xas->xa_index, max, 0, mark) : NULL;
	return xas->xa_entry;
}
void *xas_next_marked(struct xa_state *xas, unsigned int mark)
{
	if (xas->xa_index == ULONG_MAX) return NULL;
	xas->xa_index++;
	return xas_find_marked(xas, ULONG_MAX, mark);
}

void *xas_next_entries(struct xa_state *xas, int *entsp)
{
	(void)xas; (void)entsp;
	return NULL;
}

void *xas_next_chunk(struct xa_state *xas, unsigned long size)
{
	(void)xas; (void)size;
	return NULL;
}

void *xas_prev(struct xa_state *xas)
{
	if (xas->xa_index == 0)
		return NULL;
	xas->xa_index--;
	return xas_load(xas);
}

void *xas_err(struct xa_state *xas)
{
	return xas->xa_entry ? NULL : ERR_PTR(-EINVAL);
}

void *xas_fail(struct xa_state *xas)
{
	return xas->xa_entry;
}

void *xas_failed(struct xa_state *xas)
{
	return xas->xa_entry;
}

int xas_create(struct xa_state *xas, gfp_t gfp)
{
	return xas_store(xas, NULL);
}

int xas_create_range(struct xa_state *xas, unsigned long max, int order,
		     gfp_t gfp)
{
	(void)max; (void)order;
	return xas_store(xas, NULL);
}

int xas_split(struct xa_state *xas, gfp_t gfp, unsigned long order)
{
	(void)xas; (void)gfp; (void)order;
	return 0;
}

int xas_alloc(struct xa_state *xas, void *entry, gfp_t gfp)
{
	struct xarray *xa = xa_state_resolve(xas);
	unsigned long i = xas->xa_index;
	int ret;

	if (!xa)
		return -EINVAL;
	ret = xa_alloc_max(xa, &i, entry, ~0UL, gfp);
	if (!ret)
		xas->xa_index = i;
	return ret;
}

int xas_alloc_cyclic(struct xa_state *xas, void *entry,
		     unsigned long *hintp, gfp_t gfp)
{
	struct xarray *xa = xa_state_resolve(xas);
	unsigned long i = xas->xa_index;
	int ret;

	if (!xa)
		return -EINVAL;
	ret = xa_alloc_cyclic(xa, &i, entry, ~0UL, hintp, gfp);
	if (!ret)
		xas->xa_index = i;
	return ret;
}

int xas_alloc_cyclic_max(struct xa_state *xas, void *entry,
			 unsigned long *hintp, gfp_t gfp)
{
	return xas_alloc_cyclic(xas, entry, hintp, gfp);
}

int xas_alloc_reserve(struct xa_state *xas, void *entry, gfp_t gfp)
{
	return xas_alloc(xas, entry, gfp);
}

int xas_set_err(struct xa_state *xas, int error)
{
	xas->xa_entry = ERR_PTR(error);
	return error;
}

bool xa_is_value_entry(const struct xarray *xa, unsigned long index)
{
	void *e = xa_load((struct xarray *)(uintptr_t)xa, index);

	return e && xa_is_value(e);
}

/* IDR, IDA and the legacy radix API share the sparse storage. IDR callers
 * serialize reference ownership; IDA allocation/free are internally atomic. */
#include <linux/idr.h>
#include <linux/ida.h>
#include <linux/radix-tree.h>

int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	if (!idr || start < 0) return -EINVAL;
	unsigned long first = (unsigned int)start;
	unsigned long last = end > 0 ? (unsigned int)end - 1UL : INT_MAX;
	if (first < idr->idr_base) first = idr->idr_base;
	if (first > last) return -ENOSPC;
	int error = xa_alloc_max(&idr->idr_rt, &first, ptr, last, gfp);
	return error ? error : (int)first;
}

int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	if (!idr || start < 0 || IS_ERR(ptr)) return -EINVAL;
	unsigned long first = (unsigned int)start;
	unsigned long last = end > 0 ? (unsigned int)end - 1UL : INT_MAX;
	if (first < idr->idr_base) first = idr->idr_base;
	if (first > last) return -ENOSPC;
	unsigned long index, hint = idr->idr_next;
	xa_lock(&idr->idr_rt);
	pthread_mutex_lock(&xa_tree_lock);
	int error = xa_cyclic_locked(&idr->idr_rt, &index, ptr, first, last, &hint, gfp);
	if (!error) idr->idr_next = hint;
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(&idr->idr_rt);
	return error ? error : (int)index;
}

void *idr_find(struct idr *idr, unsigned int id)
{
	return idr && id >= idr->idr_base ? xa_load(&idr->idr_rt, id) : NULL;
}
void *idr_full_find(struct idr *idr, unsigned int id) { return idr_find(idr, id); }
void *idr_get_next_ul(struct idr *idr, unsigned long *next)
{
	if (!idr || !next) return NULL;
	if (*next < idr->idr_base) *next = idr->idr_base;
	return xa_find(&idr->idr_rt, next, ULONG_MAX, 0);
}
void *idr_get_next(struct idr *idr, int *next)
{
	if (!idr || !next || *next < 0) return NULL;
	unsigned long index = (unsigned int)*next;
	if (index < idr->idr_base) index = idr->idr_base;
	void *entry = xa_find(&idr->idr_rt, &index, INT_MAX, 0);
	if (entry) *next = (int)index;
	return entry;
}
void *idr_replace(struct idr *idr, void *ptr, unsigned int id)
{
	if (!idr || IS_ERR(ptr) || id < idr->idr_base) return ERR_PTR(-EINVAL);
	xa_lock(&idr->idr_rt);
	pthread_mutex_lock(&xa_tree_lock);
	void *old = xa_load_locked(&idr->idr_rt, id);
	if (old) {
		/* Replacement never allocates or releases the ID, even for NULL. */
		struct xa_node *leaf = xa_leaf(&idr->idr_rt, id);
		__atomic_store_n(&leaf->entries[id & 63], ptr ? ptr : (void *)XA_ZERO_ENTRY, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(&idr->idr_rt);
	return old ? xa_visible(old) : ERR_PTR(-ENOENT);
}
void *idr_remove(struct idr *idr, unsigned int id)
{
	return idr && id >= idr->idr_base ? xa_erase(&idr->idr_rt, id) : NULL;
}
void idr_destroy(struct idr *idr) { if (idr) xa_destroy(&idr->idr_rt); }
int idr_for_each(struct idr *idr, int (*fn)(int, void *, void *), void *data)
{
	int id = 0;
	void *entry;
	if (!idr || !fn) return -EINVAL;
	while ((entry = idr_get_next(idr, &id))) {
		int result = fn(id, entry, data);
		if (result) return result;
		if (id == INT_MAX) break;
		id++;
	}
	return 0;
}

int ida_alloc_range(struct ida *ida, unsigned int lowest, unsigned int highest, gfp_t gfp)
{
	if (!ida) return -EINVAL;
	if (highest > INT_MAX) highest = INT_MAX;
	if (lowest > highest) return -ENOSPC;
	unsigned long index = lowest;
	int error = xa_alloc_max(&ida->ia_xa, &index, xa_mk_value(1), highest, gfp);
	return error ? error : (int)index;
}
int ida_alloc(struct ida *ida, gfp_t gfp)
{ return ida_alloc_range(ida, 0, INT_MAX, gfp); }
int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp)
{ return ida_alloc_range(ida, 0, max, gfp); }
void ida_free(struct ida *ida, unsigned int id)
{ if (ida && id <= INT_MAX) xa_erase(&ida->ia_xa, id); }
void ida_destroy(struct ida *ida) { if (ida) xa_destroy(&ida->ia_xa); }

int radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *entry)
{
	if (!root || !entry || IS_ERR(entry)) return -EINVAL;
	xa_lock(root);
	pthread_mutex_lock(&xa_tree_lock);
	int error = xa_load_locked(root, index) ? -EEXIST :
		xa_store_locked(root, index, entry, root->xa_flags);
	pthread_mutex_unlock(&xa_tree_lock);
	xa_unlock(root);
	return error;
}
void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index)
{ return xa_load((struct xarray *)root, index); }
void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{ return xa_erase(root, index); }
void **radix_tree_lookup_slot(const struct radix_tree_root *root, unsigned long index)
{
	pthread_mutex_lock(&xa_tree_lock);
	struct xa_node *leaf = root ? xa_leaf((struct xarray *)root, index) : NULL;
	void **slot = leaf && leaf->entries[index & 63] ? &leaf->entries[index & 63] : NULL;
	pthread_mutex_unlock(&xa_tree_lock);
	return slot;
}
void **linuxu_radix_iter_first(struct radix_tree_root *root,
		struct radix_tree_iter *iter, unsigned long start)
{
	if (!root || !iter) return NULL;
	pthread_mutex_lock(&xa_tree_lock);
	unsigned long index = start;
	void *entry = xa_find_node(root->xa_head, 0, start, ULONG_MAX, &index, -1);
	void **slot = NULL;
	if (entry) {
		iter->index = index;
		iter->next_index = index + 1;
		iter->node = xa_leaf(root, index);
		slot = &iter->node->entries[index & 63];
	}
	pthread_mutex_unlock(&xa_tree_lock);
	return slot;
}
void **linuxu_radix_iter_next(struct radix_tree_root *root, struct radix_tree_iter *iter)
{
	return !iter || !iter->next_index ? NULL :
		linuxu_radix_iter_first(root, iter, iter->next_index);
}
int radix_tree_iter_delete(struct radix_tree_root *root, struct radix_tree_iter *iter, void **slot)
{
	(void)slot;
	return radix_tree_delete(root, iter->index) ? 0 : -ENOENT;
}
void radix_tree_tag_set(struct radix_tree_root *root, unsigned long index, unsigned long tag)
{ xa_set_mark(root, index, tag, 0); }
void radix_tree_tag_clear(struct radix_tree_root *root, unsigned long index, unsigned long tag)
{ xa_clear_mark(root, index, tag); }
int radix_tree_tag_get(struct radix_tree_root *root, unsigned long index, unsigned long tag)
{ return xa_test_mark(root, index, tag); }
int radix_tree_tagged(struct radix_tree_root *root, unsigned long tag)
{
	unsigned long index = 0;
	return xa_find_marked(root, &index, ULONG_MAX, 0, tag) != NULL;
}
int radix_tree_gang_lookup_tag(struct radix_tree_root *root, void **results,
		unsigned long first, unsigned int max, unsigned long tag)
{
	unsigned int count = 0;
	void *entry;
	if (!results) return 0;
	while (count < max && (entry = xa_find_marked(root, &first, ULONG_MAX, 0, tag))) {
		results[count++] = entry;
		if (first == ULONG_MAX) break;
		first++;
	}
	return count;
}

void radix_tree_replace_slot(struct radix_tree_root *root, void **slot, void *entry)
{
	(void)root;
	/* The caller holds its writer lock; readers may retain this leaf under RCU. */
	if (slot) __atomic_store_n(slot, entry, __ATOMIC_RELEASE);
}
void radix_tree_iter_replace(struct radix_tree_root *root,
		const struct radix_tree_iter *iter, void **slot, void *entry)
{
	(void)iter;
	radix_tree_replace_slot(root, slot, entry);
}
