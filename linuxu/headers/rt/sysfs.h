#ifndef LINUXU_RT_SYSFS_H
#define LINUXU_RT_SYSFS_H
#include <stddef.h>
/* Shared with DriverKit and C++ callers: no kernel types. */
#ifdef __cplusplus
extern "C" {
#endif
struct kobject;
/* Own an opaque in-memory directory for a registered kobject. These functions
 * perform no filesystem publication and never invoke attribute callbacks. */
int linuxu_sysfs_add(struct kobject *kobj);
void linuxu_sysfs_remove(struct kobject *kobj);
/* Cached metadata only, for offline checks and in-process diagnostics. */
int linuxu_sysfs_has_file(struct kobject *kobj, const char *group, const char *name);
size_t linuxu_sysfs_count(struct kobject *kobj);

/* Paths are relative to @root's directory, or to the top-level kobjects
 * when @root is NULL: '/'-separated names with no empty, "." or ".."
 * component, at most LINUXU_SYSFS_PATH_MAX bytes. A name resolves to an
 * entry of the current directory (attribute file, named group directory,
 * link, which is followed) or else to a child kobject's directory. */
#define LINUXU_SYSFS_PATH_MAX 1024

/* read(2) at @pos of a freshly opened sysfs file, as fs/sysfs/file.c does
 * it: an attribute file runs ktype->sysfs_ops->show() once into a zeroed
 * PAGE_SIZE buffer, a bin_attribute runs its read() for at most a page.
 * Returns the bytes copied to @buf or a negative Linux errno: show()'s or
 * read()'s own error, -ENOENT, -ENOTDIR, -EISDIR, -EACCES (no read
 * permission or no show operation), -EINVAL or -ENAMETOOLONG for a bad
 * path. @length, when given, receives the attribute's full length (show()'s
 * count, or the bin_attribute size, 0 when unknown).
 *
 * The callback runs on the caller's thread and may sleep and take upstream
 * locks, like a Linux sysfs read. Removing the file or its kobject waits
 * until the callback has returned. */
long linuxu_sysfs_read(struct kobject *root, const char *path, void *buf, size_t count,
                       long long pos, size_t *length);

/* readdir of a directory: one line per entry, "<type> <name>\n", sorted by
 * name, with type 'f' (attribute or bin file), 'd' (group or kobject
 * directory) or 'l' (link). Copies the bytes at @pos like
 * linuxu_sysfs_read; @length receives the whole listing's size. No
 * attribute callback runs. */
long linuxu_sysfs_list(struct kobject *root, const char *path, void *buf, size_t count,
                       long long pos, size_t *length);
/* A write of an attribute file, as a Linux sysfs write: its store() runs
 * once with @count bytes (less than a page) in a zeroed, NUL-terminated
 * page. Returns store()'s result (the bytes taken, or its negative errno),
 * -EACCES for a file without write permission or store operation, or a
 * binary attribute, -E2BIG for a page or more, and the path errors of
 * linuxu_sysfs_read. Runs on the caller's thread; may sleep. */
long linuxu_sysfs_write(struct kobject *root, const char *path, const void *buf, size_t count);
#ifdef __cplusplus
}
#endif
#endif
