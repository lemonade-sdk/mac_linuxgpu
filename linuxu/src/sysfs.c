/* In-process class attribute registry; no host sysfs is exposed. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/errno.h>
#include <rt/class.h>

struct class_file {
	struct class_file *next;
	const struct class *class;
	const struct attribute *attribute;
};
static pthread_mutex_t class_files_lock = PTHREAD_MUTEX_INITIALIZER;
static struct class_file *class_files;

int class_create_file(struct class *cls, const struct attribute *attr)
{
	struct class_file *r;
	struct class_file *new_file;

	if (!cls || !attr || !attr->name || !linuxu_class_is_registered(cls))
		return -EINVAL;
	new_file = malloc(sizeof(*new_file));
	if (!new_file)
		return -ENOMEM;
	pthread_mutex_lock(&class_files_lock);
	for (r = class_files; r; r = r->next)
		if (r->class == cls &&
		    strcmp(r->attribute->name, attr->name) == 0) {
			pthread_mutex_unlock(&class_files_lock);
			free(new_file);
			return -EEXIST;
		}
	new_file->class = cls;
	new_file->attribute = attr;
	new_file->next = class_files;
	class_files = new_file;
	pthread_mutex_unlock(&class_files_lock);
	return 0;
}

void class_remove_file(struct class *cls, const struct attribute *attr)
{
	struct class_file **link;
	pthread_mutex_lock(&class_files_lock);
	for (link = &class_files; *link; link = &(*link)->next)
		if ((*link)->class == cls && (*link)->attribute == attr) {
			struct class_file *r = *link;
			*link = r->next;
			free(r);
			break;
		}
	pthread_mutex_unlock(&class_files_lock);
}

void linuxu_sysfs_remove_class(const struct class *cls)
{
	struct class_file **link;
	pthread_mutex_lock(&class_files_lock);
	for (link = &class_files; *link;) {
		struct class_file *r = *link;
		if (r->class == cls) {
			*link = r->next;
			free(r);
		} else {
			link = &r->next;
		}
	}
	pthread_mutex_unlock(&class_files_lock);
}
