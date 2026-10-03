/* linuxu: SHIM (vendor/linux/include/linux/kobject.h)
 *
 * kobject: the sysfs object base that struct device embeds.
 * The driver uses &adev->dev->kobj for sysfs_create_file().
 * Runtime: linuxu/src/kmem/kobject.c
 */
#ifndef _LINUX_KOBJECT_H
#define _LINUX_KOBJECT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>

struct kobject {
	struct list_head	entry;
	struct kobject		*parent;
	struct kobject		*child;
	struct kset		*kset;
	const char		*name;
	struct kobj_type	*ktype;
	struct kobject		*root;
	void			*priv;
	refcount_t		kref;
	int			state;
	int			flags;
	const struct attribute	*attr;
	const struct attribute_group **default_groups;
	struct list_head	default_attrs;
	struct list_head	dyn_attrs;
	void			*priv_data;
	struct kobject		*parent_kobj;
	struct sysfs_dirent	*sd;
	bool			name_owned;
};

struct kobj_type {
	void			(*release)(struct kobject *kobj);
	struct kobject		*(*add)(struct kobject *kobj);
	int			(*create)(struct kobject *kobj, void **out);
	void			(*remove)(struct kobject *kobj);
	int			(*move)(struct kobject *kobj, struct kobject *new_parent);
	const char		*(*name)(struct kobject *kobj);
	const struct sysfs_ops	*sysfs_ops;
	const struct attribute_group **default_groups;
};

struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject	kobj;
	const struct kobj_type	*ktype;
};

/*
 * struct kobj_attribute - plain kobject attribute (vendor 2026,
 * verbatim). show/store take struct kobj_attribute * (NOT struct
 * attribute *); amdgpu_xcp.c / amdgpu_discovery.c initialise these via
 * __ATTR_RO/__ATTR_RW_MODE.
 */
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

extern const struct sysfs_ops kobj_sysfs_ops;

struct kobj_uevent_env {
	char *argv[3];
	char *envp[64];
	int envp_idx;
	char buf[2048];
	int buflen;
};

struct kset_uevent_ops {
	int (* const filter)(const struct kobject *kobj);
	const char *(* const name)(const struct kobject *kobj);
	int (* const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};

static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

#define KOBJ_INIT(kobj) 	do { 		INIT_LIST_HEAD(&(kobj).entry); 		(kobj).parent = NULL; 		(kobj).child = NULL; 		(kobj).kset = NULL; 		(kobj).name = NULL; 		(kobj).name_owned = false;		(kobj).sd = NULL; 		(kobj).priv = NULL; 		(kobj).ktype = NULL; 		(kobj).root = NULL; 		(kobj).priv_data = NULL; 		(kobj).parent_kobj = NULL; 		refcount_set(&(kobj).kref, 1); 		(kobj).state = 0; 		(kobj).flags = 0; 		(kobj).attr = NULL; 		INIT_LIST_HEAD(&(kobj).default_attrs); 		INIT_LIST_HEAD(&(kobj).dyn_attrs); 	} while (0)

extern void kobject_put(struct kobject *kobj);
extern int kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
extern int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
				struct kobject *parent, const char *fmt, ...);
extern int kobject_add_internal(struct kobject *kobj);
extern int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...);
extern int kobject_set_name(struct kobject *kobj, const char *fmt, ...) __printf(2, 3);
extern int kobject_set_name_vargs(struct kobject *kobj, const char *fmt, void *args);
extern const char *kobject_name(const struct kobject *kobj);
enum kobject_action {
	KOBJ_ADD = 0,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_UEVENT,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
};

extern int kobject_uevent(struct kobject *kobj, enum kobject_action action);
#define kobject_uevent_env(kobj, action, envp) \
	kobject_uevent((kobj), (action))
extern struct kobject *kobject_create(void);
extern struct kobject *kobject_create_and_add(const char *name,
					    struct kobject *parent);
extern struct kobject *kobject_create_and_add_ns(const char *name,
					    const void *ns, struct kobject *parent);
extern void kobject_cleanup(struct kobject *kobj);
extern void kobject_del(struct kobject *kobj);
extern struct kobject *kobject_get(struct kobject *kobj);
extern int kobject_rename(struct kobject *kobj, const char *new_name);
extern void kobject_set_parent(struct kobject *kobj, struct kobject *parent);
extern struct kobject *kobject_get_parent(struct kobject *kobj);
extern int kobject_set_sysfs(struct kobject *kobj);
extern int kobject_clear_sysfs(struct kobject *kobj);
extern int kobject_get_sysfs(struct kobject *kobj);
extern int kobject_put_sysfs(struct kobject *kobj);
extern int kobject_set_default_attrs(struct kobject *kobj, const struct attribute * const *attrs);
extern int kobject_clear_default_attrs(struct kobject *kobj);
extern int kobject_get_default_attrs(struct kobject *kobj);
extern int kobject_put_default_attrs(struct kobject *kobj);
extern int kobject_set_dyn_attrs(struct kobject *kobj, const struct attribute * const *attrs);
extern int kobject_clear_dyn_attrs(struct kobject *kobj);
extern int kobject_get_dyn_attrs(struct kobject *kobj);
extern int kobject_put_dyn_attrs(struct kobject *kobj);
extern int kobject_set_priv(struct kobject *kobj, void *priv);
extern void *kobject_get_priv(const struct kobject *kobj);
extern int kobject_set_state(struct kobject *kobj, int state);
extern int kobject_get_state(const struct kobject *kobj);
extern int kobject_set_flags(struct kobject *kobj, int flags);
extern int kobject_get_flags(const struct kobject *kobj);
extern int kobject_set_attr(struct kobject *kobj, const struct attribute *attr);
extern const struct attribute *kobject_get_attr(const struct kobject *kobj);
extern int kobject_set_root(struct kobject *kobj, struct kobject *root);
extern struct kobject *kobject_get_root(const struct kobject *kobj);
extern int kobject_set_kset(struct kobject *kobj, struct kset *kset);
extern struct kset *kobject_get_kset(const struct kobject *kobj);
extern int kobject_set_child(struct kobject *kobj, struct kobject *child);
extern struct kobject *kobject_get_child(const struct kobject *kobj);

/* kset API (vendor 2026; runtime: linuxu/src/kmem/kobject.c) */
extern void kset_init(struct kset *kset);
extern int kset_register(struct kset *kset);
extern void kset_unregister(struct kset *kset);
extern struct kset *kset_create_and_add(const char *name,
				       const struct kset_uevent_ops *u,
				       struct kobject *parent_kobj);
extern struct kobject *kset_find_obj(struct kset *k, const char *name);



#endif /* _LINUX_KOBJECT_H */
