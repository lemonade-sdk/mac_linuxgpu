/* linuxu shim: fw - in-process firmware cache (see fw_table.h).
 *
 * The cache is a growable array of heap records.  Embedded rows borrow
 * their process-lifetime rodata; pushed and fetched rows own a heap copy.
 * No name, size or count is tied to a particular GPU: the cache holds
 * whatever upstream requested and the host or the embedded table supplied.
 */
#include "fw_table.h"
#include "fw_rodata.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <linux/slab.h>

struct fw_entry_rec {
	/* Layout prefix matches struct fw_entry for fw_table_find(). */
	const char *name;
	const uint8_t *blob;
	size_t   size;
	int      loaded;
	int      owned;         /* host-provided copies; embedded rodata is not */
};

static struct fw_entry_rec **g_entries;
static size_t              g_count;
static size_t              g_capacity;
static pthread_mutex_t     g_lock = PTHREAD_MUTEX_INITIALIZER;

/* caller holds g_lock */
static struct fw_entry_rec *fw_find_locked(const char *name)
{
	for (size_t i = 0; i < g_count; i++)
		if (strcmp(g_entries[i]->name, name) == 0)
			return g_entries[i];
	return NULL;
}

/* caller holds g_lock; returns a fresh zeroed record appended to the table */
static struct fw_entry_rec *fw_append_locked(void)
{
	struct fw_entry_rec *rec;

	if (g_count == g_capacity) {
		size_t capacity = g_capacity ? g_capacity * 2 : 64;
		struct fw_entry_rec **grown;

		if (capacity < g_capacity ||
		    capacity > SIZE_MAX / sizeof(*g_entries))
			return NULL;
		grown = realloc(g_entries, capacity * sizeof(*g_entries));
		if (!grown)
			return NULL;
		g_entries = grown;
		g_capacity = capacity;
	}
	rec = calloc(1, sizeof(*rec));
	if (!rec)
		return NULL;
	g_entries[g_count++] = rec;
	return rec;
}

static void fw_release_rec(struct fw_entry_rec *rec)
{
	if (rec->owned) {
		free((void *)rec->name);
		free((void *)rec->blob);
	}
	rec->name = NULL;
	rec->blob = NULL;
}

/* Install an owned copy (name_copy and blob are heap pointers this table
 * now owns).  Replace-on-same-name keeps pushes idempotent. */
static int fw_install_owned(char *name_copy, uint8_t *blob, size_t size)
{
	struct fw_entry_rec *rec;

	pthread_mutex_lock(&g_lock);
	rec = fw_find_locked(name_copy);
	if (rec) {
		fw_release_rec(rec);
	} else {
		rec = fw_append_locked();
		if (!rec) {
			pthread_mutex_unlock(&g_lock);
			return -1;
		}
	}
	rec->name   = name_copy;
	rec->blob   = blob;
	rec->size   = size;
	rec->loaded = 0;
	rec->owned  = 1;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int fw_table_register(const struct fw_entry *e)
{
	char *name_copy;
	uint8_t *buf;

	if (!e || !e->name || !e->blob || e->size < 16)
		return -1;
	name_copy = strdup(e->name);
	buf = malloc(e->size);
	if (!name_copy || !buf) {
		free(name_copy);
		free(buf);
		return -1;
	}
	memcpy(buf, e->blob, e->size);
	if (fw_install_owned(name_copy, buf, e->size)) {
		free(name_copy);
		free(buf);
		return -1;
	}
	return 0;
}

int fw_table_adopt(const char *name, uint8_t *blob, size_t size)
{
	char *name_copy;

	if (!name || !*name || !blob || size < 16)
		return -1;
	name_copy = strdup(name);
	if (!name_copy)
		return -1;
	if (fw_install_owned(name_copy, blob, size)) {
		free(name_copy);
		return -1;
	}
	return 0;
}

int fw_table_has_override(const char *name)
{
	struct fw_entry_rec *rec;
	int owned;

	if (!name)
		return 0;
	pthread_mutex_lock(&g_lock);
	rec = fw_find_locked(name);
	owned = rec && rec->owned;
	pthread_mutex_unlock(&g_lock);
	return owned;
}

/* Embedded firmware has process-lifetime storage, so its record borrows the
 * rodata instead of copying it. */
static int fw_table_register_rodata(const struct fw_rodata_entry *e)
{
	struct fw_entry_rec *rec;

	if (!e || !e->name || !e->blob || e->size < 16)
		return -1;
	pthread_mutex_lock(&g_lock);
	if (fw_find_locked(e->name)) {
		/* Preserve a host-provided entry when Start is retried. */
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	rec = fw_append_locked();
	if (!rec) {
		pthread_mutex_unlock(&g_lock);
		return -1;
	}
	rec->name = e->name;
	rec->blob = e->blob;
	rec->size = e->size;
	rec->loaded = 0;
	rec->owned = 0;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int fw_table_override(const char *name, const uint8_t *bytes, size_t size)
{
	struct fw_entry e;

	if (!name || (bytes == NULL && size > 0))
		return -2;
	e.name = name;
	e.blob = bytes;
	e.size = size;
	if (size < 16)
		return -1;
	return fw_table_register(&e);
}

const struct fw_entry *fw_table_find(const char *name)
{
	struct fw_entry_rec *rec;

	if (!name)
		return NULL;
	pthread_mutex_lock(&g_lock);
	rec = fw_find_locked(name);
	pthread_mutex_unlock(&g_lock);
	return (const struct fw_entry *)rec;
}

int fw_table_snapshot(const char *name, int allow_rodata,
		      const uint8_t **blob, size_t *size, int *owned)
{
	struct fw_entry_rec *entry;
	int result = -ENOENT;

	if (!name || !blob || !size || !owned)
		return -EINVAL;
	*blob = NULL;
	*size = 0;
	*owned = 0;
	pthread_mutex_lock(&g_lock);
	entry = fw_find_locked(name);
	if (entry) {
		if (allow_rodata && !entry->owned) {
			*blob = entry->blob;
			*size = entry->size;
			result = 0;
		} else {
			uint8_t *copy = kmalloc(entry->size, GFP_KERNEL);
			if (!copy) {
				result = -ENOMEM;
			} else {
				memcpy(copy, entry->blob, entry->size);
				*blob = copy;
				*size = entry->size;
				*owned = 1;
				result = 0;
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return result;
}

int fw_table_read_buffer(const char *name, void *buffer, size_t capacity,
			 size_t offset, int partial, size_t *copied)
{
	const struct fw_entry_rec *entry;
	int result = -ENOENT;

	if (!copied) return -EINVAL;
	*copied = 0;
	if (!name || !buffer || !capacity) return -EINVAL;
	pthread_mutex_lock(&g_lock);
	entry = fw_find_locked(name);
	if (entry) {
		if (offset >= entry->size) {
			result = -EINVAL;
		} else {
			size_t length = entry->size - offset;
			if (length > capacity && !partial) {
				result = -EFBIG;
			} else {
				if (length > capacity)
					length = capacity;
				memcpy(buffer, entry->blob + offset, length);
				*copied = length;
				result = 0;
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return result;
}

size_t fw_table_count(void)
{
	size_t n;

	pthread_mutex_lock(&g_lock);
	n = g_count;
	pthread_mutex_unlock(&g_lock);
	return n;
}

const char *fw_table_name_at(size_t i)
{
	const char *n = NULL;

	pthread_mutex_lock(&g_lock);
	if (i < g_count)
		n = g_entries[i]->name;
	pthread_mutex_unlock(&g_lock);
	return n;
}

int fw_boot_load_all(void)
{
	int failures = 0;

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_count; i++) {
		if (g_entries[i]->blob == NULL || g_entries[i]->size < 16)
			failures++;
		else
			g_entries[i]->loaded = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return failures;
}

size_t fw_table_loaded_count(void)
{
	size_t n = 0;

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_count; i++)
		if (g_entries[i]->loaded)
			n++;
	pthread_mutex_unlock(&g_lock);
	return n;
}

void fw_table_clear(void)
{
	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_count; i++) {
		fw_release_rec(g_entries[i]);
		free(g_entries[i]);
	}
	free(g_entries);
	g_entries = NULL;
	g_count = 0;
	g_capacity = 0;
	pthread_mutex_unlock(&g_lock);
}

int fw_table_register_embedded(void)
{
	size_t count = 0;
	const struct fw_rodata_entry *rows;
	int failures = 0;

	rows = fw_rodata_entries(&count);
	if (!rows)
		return 0;
	for (size_t i = 0; i < count; i++) {
		if (fw_table_register_rodata(&rows[i]) != 0)
			failures++;
	}
	return failures;
}
