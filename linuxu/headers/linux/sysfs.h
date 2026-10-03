/* In-process sysfs metadata. No host filesystem or userspace control files
 * are published; registered owners retain callback declarations unchanged. */
#ifndef _LINUX_SYSFS_H
#define _LINUX_SYSFS_H
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <stddef.h>
struct kobject;
struct device;
struct file;
struct dentry;
struct vm_area_struct;
struct address_space;
struct attribute { const char *name; umode_t mode; };
struct bin_attribute {
    struct attribute attr;
    size_t size;
    void *private;
    struct address_space *(*f_mapping)(void);
    ssize_t (*read)(struct file *, struct kobject *, const struct bin_attribute *, char *, loff_t, size_t);
    ssize_t (*write)(struct file *, struct kobject *, const struct bin_attribute *, char *, loff_t, size_t);
    loff_t (*llseek)(struct file *, struct kobject *, const struct bin_attribute *, loff_t, int);
    int (*mmap)(struct file *, struct kobject *, const struct bin_attribute *, struct vm_area_struct *);
};
struct attribute_group {
    const char *name;
    union {
        umode_t (*is_visible)(struct kobject *, struct attribute *, int);
        umode_t (*is_visible_const)(struct kobject *, const struct attribute *, int);
    };
    umode_t (*is_bin_visible)(struct kobject *, const struct bin_attribute *, int);
    size_t (*bin_size)(struct kobject *, const struct bin_attribute *, int);
    union { struct attribute **attrs; const struct attribute *const *attrs_const; };
    const struct bin_attribute *const *bin_attrs;
};
struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device *, struct device_attribute *, char *);
    ssize_t (*store)(struct device *, struct device_attribute *, const char *, size_t);
};
struct sysfs_ops {
    ssize_t (*show)(struct kobject *, struct attribute *, char *);
    ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t);
};
#define SYSFS_PREALLOC 010000
#define SYSFS_GROUP_INVISIBLE 020000
#ifndef S_IRUGO
#define S_IRUGO 0444
#endif
#define __ATTR(_name, _mode, _show, _store) \
    { .attr = { .name = #_name, .mode = (_mode) }, .show = _show, .store = _store }
#define ATTR(_name, _mode, _show, _store) __ATTR(_name, _mode, _show, _store)
#define __ATTR_RO_MODE(_name, _mode) \
    { .attr = { .name = #_name, .mode = (_mode) }, .show = _name##_show }
#define __ATTR_RO(_name) __ATTR_RO_MODE(_name, 0444)
#define __ATTR_WO(_name) __ATTR(_name, 0200, NULL, _name##_store)
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_RW_MODE(_name, _mode) __ATTR(_name, _mode, _name##_show, _name##_store)
#define __ATTR_NULL { .attr = { .name = NULL } }
#define DEVICE_ATTR(_name, _mode, _show, _store) \
    struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_WO(_name) struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define DEVICE_ATTR_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define ATTRIBUTE_GROUPS(_name) \
    static const struct attribute_group _name##_group = { .attrs = _name##_attrs }; \
    static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL }
#define BIN_ATTR(_name, _mode, _read, _write, _size) \
    struct bin_attribute bin_attr_##_name = { \
        .attr = { .name = #_name, .mode = _mode }, .size = _size, .read = _read, .write = _write }

int sysfs_create_file(struct kobject *, const struct attribute *);
void sysfs_remove_file(struct kobject *, const struct attribute *);
int sysfs_create_files(struct kobject *, const struct attribute *const *);
void sysfs_remove_files(struct kobject *, const struct attribute *const *);
int sysfs_create_bin_file(struct kobject *, const struct bin_attribute *);
void sysfs_remove_bin_file(struct kobject *, const struct bin_attribute *);
int sysfs_create_group(struct kobject *, const struct attribute_group *);
int sysfs_update_group(struct kobject *, const struct attribute_group *);
void sysfs_remove_group(struct kobject *, const struct attribute_group *);
int sysfs_create_groups(struct kobject *, const struct attribute_group **);
void sysfs_remove_groups(struct kobject *, const struct attribute_group **);
int sysfs_add_file_to_group(struct kobject *, const struct attribute *, const char *);
void sysfs_remove_file_from_group(struct kobject *, const struct attribute *, const char *);
int sysfs_create_link(struct kobject *, struct kobject *, const char *);
void sysfs_remove_link(struct kobject *, const char *);
int sysfs_rename_link(struct kobject *, struct kobject *, const char *, const char *);
void sysfs_notify(struct kobject *, const char *, const char *);
void sysfs_notify_dirent(struct dentry *);
int sysfs_match_string(const char *const *, const char *);
static inline void sysfs_attr_init(struct attribute *attr) { (void)attr; }
static inline void sysfs_bin_attr_init(struct bin_attribute *attr) { (void)attr; }
static inline bool sysfs_streq(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) { ++s1; ++s2; }
    return (!*s1 && !*s2) || (*s1 == '\n' && !s1[1] && !*s2) ||
        (*s2 == '\n' && !s2[1] && !*s1);
}
struct device *kobj_to_dev(struct kobject *);
int sysfs_emit_at(char *, int, const char *, ...) __printf(3, 4);
#define sysfs_emit(buf, fmt, ...) sysfs_emit_at(buf, 0, fmt, ##__VA_ARGS__)
extern const struct sysfs_ops kobj_sysfs_ops;

/* the driver (drm_sysfs.c) declares `static CLASS_ATTR_STRING(version, ...)`
 * and uses `&class_attr_version.attr` — so the object is a struct attribute. */
struct class_attribute_shim {
	struct attribute attr;
};
#define CLASS_ATTR(name) 	static struct class_attribute_shim class_attr_##name = { .attr = { .name = #name } }
#define CLASS_ATTR_RO(name) CLASS_ATTR(name)
#define CLASS_ATTR_WO(name) CLASS_ATTR(name)
#define CLASS_ATTR_RW(name) CLASS_ATTR(name)
/* fork 2026: CLASS_ATTR_STRING yields a class attribute object (drm_sysfs.c
 * uses `&class_attr_version.attr`). */
#define CLASS_ATTR_STRING(n, permissions, strval) \
	static struct class_attribute_shim class_attr_##n = { \
		.attr = { .name = #n, .mode = (umode_t)(permissions) } \
	};

struct class_attribute {
	struct attribute attr;
	const char *(*show)(void *dev, void *vbuf, size_t size);
	void (*store)(void *dev, const char *buf, size_t size);
	ssize_t (*get)(void *dev, void *vbuf, size_t size);
};



struct class;
extern int class_create_file(struct class *cls, const struct attribute *attr);
extern void class_remove_file(struct class *cls, const struct attribute *attr);

#endif /* _LINUX_SYSFS_H */
