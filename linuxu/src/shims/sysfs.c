/* In-memory sysfs: the directory of every registered kobject (its parent
 * and name), the attribute files, bin attributes, named groups and links
 * in it, and reads that invoke the attribute exactly as fs/sysfs/file.c
 * does: ktype->sysfs_ops->show() into a zeroed PAGE_SIZE buffer, or the
 * bin_attribute's read(). No host paths, polling service or userspace
 * callback execution.
 *
 * Registration validates ownership and names, detects duplicates and rolls
 * back errors; kobject teardown removes every entry. Names are copied so
 * teardown never dereferences expired attribute objects. Attribute pointers
 * are dereferenced only by a read, which holds the entry active; removal
 * unlinks the entry and waits for active reads to leave it, as kernfs
 * deactivation does, so show()/read() never runs after its file was
 * removed or its kobject released. */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <rt/sysfs.h>

#define SYSFS_MAX_ATTRS 2048
#define SYSFS_NAME_MAX 255
#define SYSFS_LIST_MAX (1u << 20)
/* One per registered kobject (kobj->sd). parent is the directory the kobject
 * was added under; NULL for a top-level kobject. An orphan's parent was
 * removed first, so no path reaches it. */
struct sysfs_dirent {
    struct sysfs_dirent *next;
    struct kobject *kobj, *parent;
    bool orphan;
};
enum sysfs_kind { SYSFS_FILE, SYSFS_BIN, SYSFS_LINK, SYSFS_GROUP };
struct sysfs_rec {
    struct sysfs_rec *next;
    struct kobject *kobj, *target;
    const struct attribute *attr;       /* SYSFS_FILE */
    const struct bin_attribute *bin;    /* SYSFS_BIN */
    size_t size;                        /* SYSFS_BIN: file size, 0 unknown */
    enum sysfs_kind kind;
    umode_t mode;
    unsigned int active;                /* reads inside show()/read() */
    char name[SYSFS_NAME_MAX + 1], group[SYSFS_NAME_MAX + 1];
};
static struct sysfs_rec *sysfs_recs;
static struct sysfs_dirent *sysfs_dirs;
static size_t sysfs_count;
static pthread_mutex_t sysfs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sysfs_drained = PTHREAD_COND_INITIALIZER;
static int valid_name(const char *name)
{
    return name && *name && strlen(name) <= SYSFS_NAME_MAX &&
        !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}
static struct sysfs_rec *record_new(struct kobject *kobj, const char *group,
                                    const char *name, enum sysfs_kind kind, umode_t mode)
{
    struct sysfs_rec *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->kobj = kobj; r->kind = kind; r->mode = mode;
    strcpy(r->name, name);
    if (group) strcpy(r->group, group);
    return r;
}
static void records_free(struct sysfs_rec *r)
{
    while (r) { struct sysfs_rec *next = r->next; free(r); r = next; }
}
/* Free unlinked entries once no read is inside their callback. Called and
 * returns with sysfs_lock held. */
static void records_retire_locked(struct sysfs_rec *dying)
{
    for (;;) {
        bool busy = false;
        for (struct sysfs_rec *r = dying; r && !busy; r = r->next) busy = r->active != 0;
        if (!busy) break;
        pthread_cond_wait(&sysfs_drained, &sysfs_lock);
    }
    records_free(dying);
}
static int same_name(const struct sysfs_rec *a, const struct sysfs_rec *b)
{
    return a->kobj == b->kobj && !strcmp(a->group, b->group) && !strcmp(a->name, b->name);
}
static int in_group(const struct sysfs_rec *r, struct kobject *kobj, const char *group)
{
    if (r->kobj != kobj) return 0;
    return !strcmp(r->group, group ? group : "") ||
        (group && r->kind == SYSFS_GROUP && !*r->group && !strcmp(r->name, group));
}
static int update_owns(const struct sysfs_rec *r, struct kobject *kobj,
                       const struct attribute_group *group)
{
    if (!group || r->kobj != kobj) return 0;
    if (group->name) return in_group(r, kobj, group->name);
    if (*r->group) return 0;
    if (group->attrs_const) for (size_t i = 0; group->attrs_const[i]; ++i)
        if (!strcmp(r->name, group->attrs_const[i]->name)) return 1;
    if (group->bin_attrs) for (size_t i = 0; group->bin_attrs[i]; ++i)
        if (!strcmp(r->name, group->bin_attrs[i]->attr.name)) return 1;
    return 0;
}
/* Publish one staged transaction. No callbacks run while holding the lock. */
static int records_publish(struct kobject *kobj, struct sysfs_rec *staged, const struct attribute_group *update)
{
    size_t count = 0, replaced = 0;
    int result = 0;
    pthread_mutex_lock(&sysfs_lock);
    if (!kobj->sd) result = -ENOENT;
    for (struct sysfs_rec *r = sysfs_recs; r; r = r->next)
        if (update_owns(r, kobj, update)) ++replaced;
    for (struct sysfs_rec *r = staged; !result && r; r = r->next) {
        ++count;
        if (r->target && !r->target->sd) { result = -ENOENT; break; }
        if (*r->group) {
            int directory = 0;
            for (struct sysfs_rec *g = sysfs_recs; g; g = g->next)
                if (g->kobj == kobj && g->kind == SYSFS_GROUP && !strcmp(g->name, r->group)) { directory = 1; break; }
            for (struct sysfs_rec *g = staged; !directory && g; g = g->next)
                if (g->kind == SYSFS_GROUP && !strcmp(g->name, r->group)) directory = 1;
            if (!directory) { result = -ENOENT; break; }
        }
        for (struct sysfs_rec *other = sysfs_recs; other; other = other->next)
            if (same_name(r, other) && !update_owns(other, kobj, update)) { result = -EEXIST; break; }
        for (struct sysfs_rec *other = r->next; !result && other; other = other->next)
            if (same_name(r, other)) result = -EEXIST;
    }
    if (!result && count > SYSFS_MAX_ATTRS - (sysfs_count - replaced)) result = -ENOSPC;
    struct sysfs_rec *dying = NULL;
    if (!result && update) {
        struct sysfs_rec **link = &sysfs_recs;
        while (*link) {
            struct sysfs_rec *r = *link;
            if (update_owns(r, kobj, update)) {
                *link = r->next; --sysfs_count; r->next = dying; dying = r;
            } else link = &r->next;
        }
    }
    if (!result) while (staged) {
        struct sysfs_rec *next = staged->next;
        staged->next = sysfs_recs; sysfs_recs = staged; staged = next; ++sysfs_count;
    }
    records_retire_locked(dying);
    pthread_mutex_unlock(&sysfs_lock);
    records_free(staged);
    return result;
}
int linuxu_sysfs_add(struct kobject *kobj)
{
    if (!kobj) return -EINVAL;
    struct sysfs_dirent *dir = calloc(1, sizeof(*dir));
    if (!dir) return -ENOMEM;
    pthread_mutex_lock(&sysfs_lock);
    if (kobj->sd) { pthread_mutex_unlock(&sysfs_lock); free(dir); return -EEXIST; }
    struct kobject *parent = kobj->parent ? kobj->parent : kobj->kset ? &kobj->kset->kobj : NULL;
    if (parent && !parent->sd) { pthread_mutex_unlock(&sysfs_lock); free(dir); return -ENOENT; }
    dir->kobj = kobj; dir->parent = parent;
    dir->next = sysfs_dirs; sysfs_dirs = dir;
    kobj->sd = dir;
    pthread_mutex_unlock(&sysfs_lock);
    return 0;
}
void linuxu_sysfs_remove(struct kobject *kobj)
{
    if (!kobj) return;
    pthread_mutex_lock(&sysfs_lock);
    struct sysfs_dirent *dir = kobj->sd; kobj->sd = NULL;
    struct sysfs_rec *dying = NULL, **link = &sysfs_recs;
    while (*link) {
        struct sysfs_rec *r = *link;
        if (r->kobj == kobj || r->target == kobj) {
            *link = r->next; --sysfs_count; r->next = dying; dying = r;
        } else link = &r->next;
    }
    if (dir) {
        for (struct sysfs_dirent **d = &sysfs_dirs; *d; d = &(*d)->next)
            if (*d == dir) { *d = dir->next; break; }
        for (struct sysfs_dirent *d = sysfs_dirs; d; d = d->next)
            if (d->parent == kobj) { d->parent = NULL; d->orphan = true; }
    }
    records_retire_locked(dying);
    pthread_mutex_unlock(&sysfs_lock);
    free(dir);
}
int linuxu_sysfs_has_file(struct kobject *kobj, const char *group, const char *name)
{
    int found = 0;
    if (!kobj || !name) return 0;
    pthread_mutex_lock(&sysfs_lock);
    for (struct sysfs_rec *r = sysfs_recs; r; r = r->next)
        if (r->kobj == kobj && r->kind != SYSFS_GROUP && !strcmp(r->group, group ? group : "") && !strcmp(r->name, name)) { found = 1; break; }
    pthread_mutex_unlock(&sysfs_lock);
    return found;
}
size_t linuxu_sysfs_count(struct kobject *kobj)
{
    size_t count = 0;
    pthread_mutex_lock(&sysfs_lock);
    for (struct sysfs_rec *r = sysfs_recs; r; r = r->next)
        if (!kobj || r->kobj == kobj) ++count;
    pthread_mutex_unlock(&sysfs_lock);
    return count;
}
static int create_attr(struct kobject *kobj, const struct attribute *attr,
                       const struct bin_attribute *bin, const char *group)
{
    if (!kobj || !attr || !valid_name(attr->name) || (group && !valid_name(group))) return -EINVAL;
    struct sysfs_rec *r = record_new(kobj, group, attr->name, bin ? SYSFS_BIN : SYSFS_FILE, attr->mode);
    if (!r) return -ENOMEM;
    r->attr = attr; r->bin = bin; r->size = bin ? bin->size : 0;
    if (group) {
        int found = 0;
        pthread_mutex_lock(&sysfs_lock);
        for (struct sysfs_rec *g = sysfs_recs; g; g = g->next)
            if (g->kobj == kobj && g->kind == SYSFS_GROUP && !strcmp(g->name, group)) { found = 1; break; }
        pthread_mutex_unlock(&sysfs_lock);
        if (!found) { free(r); return -ENOENT; }
    }
    return records_publish(kobj, r, NULL);
}
static void remove_name(struct kobject *kobj, const char *group, const char *name)
{
    if (!kobj || !name) return;
    pthread_mutex_lock(&sysfs_lock);
    struct sysfs_rec *dying = NULL, **link = &sysfs_recs;
    while (*link) {
        struct sysfs_rec *r = *link;
        if (r->kobj == kobj && !strcmp(r->group, group ? group : "") && !strcmp(r->name, name)) {
            *link = r->next; --sysfs_count; r->next = NULL; dying = r; break;
        }
        link = &r->next;
    }
    records_retire_locked(dying);
    pthread_mutex_unlock(&sysfs_lock);
}
int sysfs_create_file(struct kobject *kobj, const struct attribute *attr) { return create_attr(kobj, attr, NULL, NULL); }
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr) { if (attr) remove_name(kobj, NULL, attr->name); }
int device_create_file(struct device *dev, const struct device_attribute *attr) { return dev ? sysfs_create_file(&dev->kobj, attr ? &attr->attr : NULL) : -EINVAL; }
void device_remove_file(struct device *dev, const struct device_attribute *attr) { if (dev && attr) sysfs_remove_file(&dev->kobj, &attr->attr); }
int sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr) { return create_attr(kobj, attr ? &attr->attr : NULL, attr, NULL); }
void sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr) { if (attr) remove_name(kobj, NULL, attr->attr.name); }
int device_create_bin_file(struct device *dev, const struct bin_attribute *attr) { return dev ? sysfs_create_bin_file(&dev->kobj, attr) : -EINVAL; }
void device_remove_bin_file(struct device *dev, const struct bin_attribute *attr) { if (dev) sysfs_remove_bin_file(&dev->kobj, attr); }
int sysfs_add_file_to_group(struct kobject *kobj, const struct attribute *attr, const char *group) { return create_attr(kobj, attr, NULL, group); }
void sysfs_remove_file_from_group(struct kobject *kobj, const struct attribute *attr, const char *group) { if (attr) remove_name(kobj, group, attr->name); }
int sysfs_create_files(struct kobject *kobj, const struct attribute *const *attrs)
{
    if (!kobj || !attrs) return -EINVAL;
    struct sysfs_rec *staged = NULL;
    for (size_t i = 0; attrs[i]; ++i) {
        int error = i >= SYSFS_MAX_ATTRS ? -ENOSPC : !valid_name(attrs[i]->name) ? -EINVAL : 0;
        if (error) { records_free(staged); return error; }
        struct sysfs_rec *r = record_new(kobj, NULL, attrs[i]->name, SYSFS_FILE, attrs[i]->mode);
        if (!r) { records_free(staged); return -ENOMEM; }
        r->attr = attrs[i];
        r->next = staged; staged = r;
    }
    return records_publish(kobj, staged, NULL);
}
void sysfs_remove_files(struct kobject *kobj, const struct attribute *const *attrs)
{
    if (attrs) for (size_t i = 0; attrs[i]; ++i) sysfs_remove_file(kobj, attrs[i]);
}
/* fs/sysfs/group.c: is_visible()/is_bin_visible() decide each file's mode
 * once, at creation; bin_size() its size. */
static int group_apply(struct kobject *kobj, const struct attribute_group *group, int update)
{
    if (!kobj || !group || (group->name && !valid_name(group->name))) return -EINVAL;
    pthread_mutex_lock(&sysfs_lock);
    int registered = kobj->sd != NULL;
    pthread_mutex_unlock(&sysfs_lock);
    if (!registered) return -ENOENT;
    struct sysfs_rec *staged = NULL;
    size_t count = 0;
    if (group->name) {
        staged = record_new(kobj, NULL, group->name, SYSFS_GROUP, 0755);
        if (!staged) return -ENOMEM;
    }
    for (int binary = 0; binary < 2; ++binary) {
        if (binary ? !group->bin_attrs : !group->attrs) continue;
        for (size_t i = 0; ; ++i) {
            const struct bin_attribute *bin = binary ? group->bin_attrs[i] : NULL;
            const struct attribute *attr = binary ? (bin ? &bin->attr : NULL) : group->attrs_const[i];
            if (!attr) break;
            if (++count > SYSFS_MAX_ATTRS || !valid_name(attr->name)) { records_free(staged); return count > SYSFS_MAX_ATTRS ? -ENOSPC : -EINVAL; }
            umode_t mode = attr->mode;
            if (binary && group->is_bin_visible) mode = group->is_bin_visible(kobj, bin, (int)i);
            if (!binary && group->is_visible_const) mode = group->is_visible_const(kobj, attr, (int)i);
            if (mode & SYSFS_GROUP_INVISIBLE) { records_free(staged); return records_publish(kobj, NULL, update ? group : NULL); }
            if (!mode) continue;
            struct sysfs_rec *r = record_new(kobj, group->name, attr->name, binary ? SYSFS_BIN : SYSFS_FILE, mode);
            if (!r) { records_free(staged); return -ENOMEM; }
            r->attr = attr; r->bin = bin;
            if (bin) r->size = group->bin_size ? group->bin_size(kobj, bin, (int)i) : bin->size;
            r->next = staged; staged = r;
        }
    }
    return records_publish(kobj, staged, update ? group : NULL);
}
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *group) { return group_apply(kobj, group, 0); }
int sysfs_update_group(struct kobject *kobj, const struct attribute_group *group) { return group_apply(kobj, group, 1); }
void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *group)
{
    if (!kobj || !group) return;
    if (!group->name) {
        if (group->attrs_const) sysfs_remove_files(kobj, group->attrs_const);
        if (group->bin_attrs) for (size_t i = 0; group->bin_attrs[i]; ++i) sysfs_remove_bin_file(kobj, group->bin_attrs[i]);
        return;
    }
    pthread_mutex_lock(&sysfs_lock);
    struct sysfs_rec *dying = NULL, **link = &sysfs_recs;
    while (*link) {
        struct sysfs_rec *r = *link;
        if (in_group(r, kobj, group->name)) { *link = r->next; --sysfs_count; r->next = dying; dying = r; }
        else link = &r->next;
    }
    records_retire_locked(dying);
    pthread_mutex_unlock(&sysfs_lock);
}
/* fs/sysfs/group.c: create in order, unwind the created ones on failure. */
int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    if (!groups) return 0;
    for (size_t i = 0; groups[i]; ++i) {
        int error = sysfs_create_group(kobj, groups[i]);
        if (error) {
            while (i--) sysfs_remove_group(kobj, groups[i]);
            return error;
        }
    }
    return 0;
}
void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    if (groups) for (size_t i = 0; groups[i]; ++i) sysfs_remove_group(kobj, groups[i]);
}
int device_add_groups(struct device *dev, const struct attribute_group **groups)
{
    return dev ? sysfs_create_groups(&dev->kobj, groups) : -EINVAL;
}
void device_remove_groups(struct device *dev, const struct attribute_group **groups)
{
    if (dev) sysfs_remove_groups(&dev->kobj, groups);
}
int sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name)
{
    if (!kobj || !target || !valid_name(name)) return -EINVAL;
    struct sysfs_rec *r = record_new(kobj, NULL, name, SYSFS_LINK, 0777);
    if (!r) return -ENOMEM;
    r->target = target;
    return records_publish(kobj, r, NULL);
}
void sysfs_remove_link(struct kobject *kobj, const char *name) { remove_name(kobj, NULL, name); }
int sysfs_rename_link(struct kobject *kobj, struct kobject *target, const char *old, const char *name)
{
    if (!kobj || !target || !valid_name(old) || !valid_name(name)) return -EINVAL;
    int result = -ENOENT;
    pthread_mutex_lock(&sysfs_lock);
    struct sysfs_rec *found = NULL;
    for (struct sysfs_rec *r = sysfs_recs; r; r = r->next) {
        if (r->kobj != kobj || *r->group) continue;
        if (!strcmp(r->name, name)) { result = -EEXIST; found = NULL; break; }
        if (r->kind == SYSFS_LINK && r->target == target && !strcmp(r->name, old)) found = r;
    }
    if (found) { strcpy(found->name, name); result = 0; }
    pthread_mutex_unlock(&sysfs_lock);
    return result;
}

/* ---- path walk, read and directory listing ----
 *
 * A path is relative to @root, or to the top-level kobjects when @root is
 * NULL: '/'-separated names, no empty, "." or ".." component. Each name is
 * looked up as an entry of the current directory (file, group directory or
 * link, which is followed) and then as a child kobject. Group directories
 * hold only files. */
struct sysfs_where {
    struct kobject *kobj;        /* the directory's kobject; NULL: top level */
    const char *group;           /* a named group directory in it, or NULL */
    struct sysfs_rec *file;      /* the file the path names, or NULL */
};
static struct sysfs_rec *entry_locked(struct kobject *kobj, const char *group, const char *name)
{
    if (!kobj) return NULL;
    for (struct sysfs_rec *r = sysfs_recs; r; r = r->next)
        if (r->kobj == kobj && !strcmp(r->group, group ? group : "") && !strcmp(r->name, name))
            return r;
    return NULL;
}
static struct kobject *child_locked(struct kobject *parent, const char *name)
{
    for (struct sysfs_dirent *d = sysfs_dirs; d; d = d->next)
        if (!d->orphan && d->parent == parent && d->kobj->name && !strcmp(d->kobj->name, name))
            return d->kobj;
    return NULL;
}
static int walk_locked(struct kobject *root, const char *path, struct sysfs_where *at)
{
    char name[SYSFS_NAME_MAX + 1];
    size_t length = strnlen(path, LINUXU_SYSFS_PATH_MAX + 1);
    if (length > LINUXU_SYSFS_PATH_MAX) return -ENAMETOOLONG;
    if (root && !root->sd) return -ENOENT;
    *at = (struct sysfs_where){ .kobj = root };
    const char *p = path;
    while (length && *p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return -EINVAL;
        if (n > SYSFS_NAME_MAX) return -ENAMETOOLONG;
        if (at->file) return -ENOTDIR;
        memcpy(name, p, n); name[n] = 0;
        struct sysfs_rec *r = entry_locked(at->kobj, at->group, name);
        if (r && (r->kind == SYSFS_FILE || r->kind == SYSFS_BIN)) {
            at->file = r;
        } else if (r && r->kind == SYSFS_GROUP) {
            at->group = r->name;
        } else if (r && r->kind == SYSFS_LINK) {
            if (!r->target->sd) return -ENOENT;
            at->kobj = r->target;
        } else {
            struct kobject *child = at->group ? NULL : child_locked(at->kobj, name);
            if (!child) return -ENOENT;
            at->kobj = child;
        }
        if (!end) break;
        p = end + 1;
        if (!*p) return -EINVAL;
    }
    return 0;
}
static void read_done(struct sysfs_rec *r)
{
    pthread_mutex_lock(&sysfs_lock);
    if (!--r->active) pthread_cond_broadcast(&sysfs_drained);
    pthread_mutex_unlock(&sysfs_lock);
}
/* fs/sysfs/file.c sysfs_kf_seq_show: one show() into a zeroed page; a
 * count of PAGE_SIZE or more is clamped to PAGE_SIZE - 1. */
static ssize_t show_read(struct sysfs_rec *r, char *buf, size_t count, loff_t pos, size_t *length)
{
    const struct sysfs_ops *ops = r->kobj->ktype ? r->kobj->ktype->sysfs_ops : NULL;
    if (!ops || !ops->show) return -EACCES;
    char *page = calloc(1, PAGE_SIZE);
    if (!page) return -ENOMEM;
    ssize_t shown = ops->show(r->kobj, (struct attribute *)r->attr, page);
    if (shown < 0) { free(page); return shown; }
    if (shown >= (ssize_t)PAGE_SIZE) shown = PAGE_SIZE - 1;
    if (length) *length = (size_t)shown;
    size_t copied = 0;
    if (pos < shown) {
        copied = (size_t)(shown - pos) < count ? (size_t)(shown - pos) : count;
        memcpy(buf, page + pos, copied);
    }
    free(page);
    return (ssize_t)copied;
}
/* sysfs_kf_bin_read: clamp to the file size, at most a page per read. */
static ssize_t bin_read(struct sysfs_rec *r, char *buf, size_t count, loff_t pos, size_t *length)
{
    if (length) *length = r->size;
    if (r->size) {
        if ((size_t)pos >= r->size) return 0;
        if (count > r->size - (size_t)pos) count = r->size - (size_t)pos;
    }
    if (!r->bin->read) return -EIO;
    if (count > PAGE_SIZE) count = PAGE_SIZE;
    if (!count) return 0;
    char *page = calloc(1, count);
    if (!page) return -ENOMEM;
    ssize_t got = r->bin->read(NULL, r->kobj, r->bin, page, pos, count);
    if (got > (ssize_t)count) got = (ssize_t)count;
    if (got > 0) memcpy(buf, page, (size_t)got);
    free(page);
    return got;
}
long linuxu_sysfs_read(struct kobject *root, const char *path, void *buf, size_t count,
                       long long pos, size_t *length)
{
    struct sysfs_where at;
    if (length) *length = 0;
    if (!path || (!buf && count) || pos < 0) return -EINVAL;
    pthread_mutex_lock(&sysfs_lock);
    int error = walk_locked(root, path, &at);
    if (!error && !at.file) error = -EISDIR;
    /* kernfs_fop_open: reading needs a read permission bit. */
    if (!error && !(at.file->mode & 0444)) error = -EACCES;
    if (!error) ++at.file->active;
    pthread_mutex_unlock(&sysfs_lock);
    if (error) return error;
    ssize_t result = at.file->kind == SYSFS_BIN ? bin_read(at.file, buf, count, pos, length)
                                                : show_read(at.file, buf, count, pos, length);
    read_done(at.file);
    return result;
}
/* fs/sysfs/file.c sysfs_kf_write: one store() of @count bytes, from a
 * zeroed page so the buffer is NUL-terminated as kernfs makes it. Binary
 * attributes are not written. */
long linuxu_sysfs_write(struct kobject *root, const char *path, const void *buf, size_t count)
{
    struct sysfs_where at;
    if (!path || !buf || !count) return -EINVAL;
    if (count >= PAGE_SIZE) return -E2BIG;
    pthread_mutex_lock(&sysfs_lock);
    int error = walk_locked(root, path, &at);
    if (!error && !at.file) error = -EISDIR;
    /* kernfs_fop_open: writing needs a write permission bit. */
    if (!error && (at.file->kind == SYSFS_BIN || !(at.file->mode & 0222))) error = -EACCES;
    if (!error) ++at.file->active;
    pthread_mutex_unlock(&sysfs_lock);
    if (error) return error;
    const struct sysfs_ops *ops = at.file->kobj->ktype ? at.file->kobj->ktype->sysfs_ops : NULL;
    ssize_t result = -EACCES;
    if (ops && ops->store) {
        char *page = calloc(1, PAGE_SIZE);
        result = -ENOMEM;
        if (page) {
            memcpy(page, buf, count);
            result = ops->store(at.file->kobj, (struct attribute *)at.file->attr, page, count);
            free(page);
        }
    }
    read_done(at.file);
    return result;
}
struct sysfs_listing { char type; const char *name; };
static int listing_order(const void *a, const void *b)
{
    return strcmp(((const struct sysfs_listing *)a)->name, ((const struct sysfs_listing *)b)->name);
}
/* Built under the lock, so entry and kobject names stay valid. */
static ssize_t list_locked(const struct sysfs_where *at, char **text)
{
    size_t entries = 0, used = 0, capacity = 0;
    struct sysfs_listing *list = NULL;
    for (int pass = 0; pass < 2; ++pass) {
        size_t n = 0;
        if (at->kobj) for (struct sysfs_rec *r = sysfs_recs; r; r = r->next) {
            if (r->kobj != at->kobj || strcmp(r->group, at->group ? at->group : "")) continue;
            if (pass) list[n] = (struct sysfs_listing){
                r->kind == SYSFS_GROUP ? 'd' : r->kind == SYSFS_LINK ? 'l' : 'f', r->name };
            ++n;
        }
        if (!at->group) for (struct sysfs_dirent *d = sysfs_dirs; d; d = d->next) {
            if (d->orphan || d->parent != at->kobj || !d->kobj->name) continue;
            if (pass) list[n] = (struct sysfs_listing){ 'd', d->kobj->name };
            ++n;
        }
        if (!pass) {
            entries = n;
            list = calloc(entries ? entries : 1, sizeof(*list));
            if (!list) return -ENOMEM;
        }
    }
    qsort(list, entries, sizeof(*list), listing_order);
    for (size_t i = 0; i < entries; ++i) capacity += strlen(list[i].name) + 3;
    if (capacity > SYSFS_LIST_MAX) { free(list); return -E2BIG; }
    *text = malloc(capacity ? capacity : 1);
    if (!*text) { free(list); return -ENOMEM; }
    for (size_t i = 0; i < entries; ++i) {
        size_t n = strlen(list[i].name);
        (*text)[used++] = list[i].type;
        (*text)[used++] = ' ';
        memcpy(*text + used, list[i].name, n);
        used += n;
        (*text)[used++] = '\n';
    }
    free(list);
    return (ssize_t)used;
}
long linuxu_sysfs_list(struct kobject *root, const char *path, void *buf, size_t count,
                       long long pos, size_t *length)
{
    struct sysfs_where at;
    char *text = NULL;
    if (length) *length = 0;
    if (!path || (!buf && count) || pos < 0) return -EINVAL;
    pthread_mutex_lock(&sysfs_lock);
    ssize_t total = walk_locked(root, path, &at);
    if (!total && at.file) total = -ENOTDIR;
    if (!total) total = list_locked(&at, &text);
    pthread_mutex_unlock(&sysfs_lock);
    if (total < 0) return total;
    if (length) *length = (size_t)total;
    size_t copied = 0;
    if (pos < total) {
        copied = (size_t)(total - pos) < count ? (size_t)(total - pos) : count;
        memcpy(buf, text + pos, copied);
    }
    free(text);
    return (ssize_t)copied;
}
int sysfs_match_string(const char *const *strings, const char *text)
{
    if (!strings || !text) return -EINVAL;
    for (int i = 0; strings[i]; ++i) if (!strcmp(strings[i], text)) return i;
    return -EINVAL;
}
void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr) { (void)kobj; (void)dir; (void)attr; }
void sysfs_notify_dirent(struct dentry *entry) { (void)entry; }
struct device *kobj_to_dev(struct kobject *kobj) { return kobj ? container_of(kobj, struct device, kobj) : NULL; }
int sysfs_emit_at(char *buf, int at, const char *format, ...)
{
    if (!buf || !format || at < 0 || at >= PAGE_SIZE) return 0;
    va_list args; va_start(args, format);
    int count = vsnprintf(buf + at, PAGE_SIZE - at, format, args);
    va_end(args);
    if (count < 0) return 0;
    return count < PAGE_SIZE - at ? count : PAGE_SIZE - at - 1;
}

/* lib/kobject.c kobj_sysfs_ops (amdgpu_xcp.c / amdgpu_discovery.c ktypes,
 * dynamic kobjects and ksets): route to the kobj_attribute callbacks. */
static ssize_t kobj_sysfs_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
	struct kobj_attribute *ka =
		container_of(attr, struct kobj_attribute, attr);

	if (ka->show)
		return ka->show(kobj, ka, buf);
	return -EIO;
}

static ssize_t kobj_sysfs_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	struct kobj_attribute *ka =
		container_of(attr, struct kobj_attribute, attr);

	if (ka->store)
		return ka->store(kobj, ka, buf, count);
	return -EIO;
}

const struct sysfs_ops kobj_sysfs_ops = {
	.show = kobj_sysfs_show,
	.store = kobj_sysfs_store,
};

/*
 * time64_to_tm — kernel copy of gmtime_r (vendor 2026 time.h; amdgpu_cper.c
 * CPER timestamps are UTC, offset 0).
 */
#include <linux/time.h>

void time64_to_tm(time64_t totalsecs, int offset, struct tm *result)
{
	static const u32 month_len[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	time64_t days = (totalsecs - offset) / 86400;
	time64_t year = 1970;
	u32 m;

	result->tm_sec = (int)(totalsecs % 60);
	result->tm_min = (int)((totalsecs / 60) % 60);
	result->tm_hour = (int)((totalsecs / 3600) % 24);
	if (days < 0)
		days = 0;
	while (1) {
		u32 ylen = 365 + (year % 4 == 0 && (year % 100 != 0 ||
						    year % 400 == 0));
		if (days < ylen)
			break;
		days -= ylen;
		year++;
	}
	result->tm_year = (long)(year - 1900);
	result->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
	for (m = 0; m < 12; m++) {
		u32 len = month_len[m];

		if (m == 1 && year % 4 == 0 &&
		    (year % 100 != 0 || year % 400 == 0))
			len = 29;
		if (days < len)
			break;
		days -= len;
	}
	result->tm_mon = (int)m;
	result->tm_mday = (int)days + 1;
	result->tm_yday = (int)(days + m * 0); /* approx; not used by the KMD */
	for (m = 0; m < (u32)result->tm_mon; m++)
		result->tm_yday += month_len[m];
	result->tm_isdst = 0;
	result->tm_gmtoff = offset;
	result->tm_zone = "UTC";
}
