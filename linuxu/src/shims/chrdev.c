/* In-process Linux character-device registration for DRM and KFD. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/chardev.h>
#include <linux/errno.h>
#include <rt/chrdev.h>

#define LINUXU_CHRDEV_MAX_MAJOR 512U
#define LINUXU_CHRDEV_LEGACY_MINORS 256U

struct chrdev_registration {
	struct chrdev_registration *next;
	unsigned int major;
	unsigned int first_minor;
	unsigned int count;
	const struct file_operations *fops;
	char name[64];
};

static pthread_mutex_t chrdev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct chrdev_registration *chrdevs;

static bool overlaps(unsigned int major, unsigned int first, unsigned int count)
{
	struct chrdev_registration *r;

	for (r = chrdevs; r; r = r->next)
		if (r->major == major && first < r->first_minor + r->count &&
		    r->first_minor < first + count)
			return true;
	return false;
}

static unsigned int dynamic_major(void)
{
	unsigned int major;

	/* Same preferred major ranges and search order as Linux fs/char_dev.c. */
	for (major = 254; major >= 234; major--)
		if (!overlaps(major, 0, MINORMASK + 1U))
			return major;
	for (major = 511; major >= 384; major--)
		if (!overlaps(major, 0, MINORMASK + 1U))
			return major;
	return 0;
}

static struct chrdev_registration *new_registration(unsigned int major,
		unsigned int first, unsigned int count, const char *name,
		const struct file_operations *fops)
{
	struct chrdev_registration *r = malloc(sizeof(*r));
	size_t len;

	if (!r)
		return NULL;
	r->next = NULL;
	r->major = major;
	r->first_minor = first;
	r->count = count;
	r->fops = fops;
	len = strlen(name);
	if (len >= sizeof(r->name))
		len = sizeof(r->name) - 1;
	memcpy(r->name, name, len);
	r->name[len] = '\0';
	return r;
}

static int register_one(unsigned int major, unsigned int first,
		unsigned int count, const char *name,
		const struct file_operations *fops)
{
	struct chrdev_registration *r;

	if (!name || !count || major >= LINUXU_CHRDEV_MAX_MAJOR ||
	    first > MINORMASK || count > MINORMASK + 1U - first)
		return -EINVAL;
	if (overlaps(major, first, count))
		return -EBUSY;
	r = new_registration(major, first, count, name, fops);
	if (!r)
		return -ENOMEM;
	r->next = chrdevs;
	chrdevs = r;
	return 0;
}

static void unregister_one(unsigned int major, unsigned int first,
		unsigned int count, bool require_fops)
{
	struct chrdev_registration **link = &chrdevs;

	while (*link) {
		struct chrdev_registration *r = *link;
		if (r->major == major && r->first_minor == first &&
		    r->count == count && (!require_fops || r->fops)) {
			*link = r->next;
			free(r);
			return;
		}
		link = &r->next;
	}
}

int register_chrdev(unsigned int major, const char *name,
		    const struct file_operations *fops)
{
	int ret;
	unsigned int chosen = major;

	if (!name || !fops || major >= LINUXU_CHRDEV_MAX_MAJOR)
		return -EINVAL;
	pthread_mutex_lock(&chrdev_lock);
	if (!chosen) {
		chosen = dynamic_major();
		if (!chosen) {
			pthread_mutex_unlock(&chrdev_lock);
			return -EBUSY;
		}
	}
	ret = register_one(chosen, 0, LINUXU_CHRDEV_LEGACY_MINORS,
			   name, fops);
	pthread_mutex_unlock(&chrdev_lock);
	return ret ? ret : (major ? 0 : (int)chosen);
}

void unregister_chrdev(unsigned int major, const char *name)
{
	(void)name; /* Linux unregister_chrdev also identifies by major/range. */
	if (!major || major >= LINUXU_CHRDEV_MAX_MAJOR)
		return;
	pthread_mutex_lock(&chrdev_lock);
	unregister_one(major, 0, LINUXU_CHRDEV_LEGACY_MINORS, true);
	pthread_mutex_unlock(&chrdev_lock);
}

int alloc_chrdev_region(dev_t *dev, unsigned int baseminor,
			unsigned int count, const char *name)
{
	unsigned int major;
	int ret;

	if (!dev || !name || !count || baseminor > MINORMASK ||
	    count > MINORMASK + 1U - baseminor)
		return -EINVAL;
	pthread_mutex_lock(&chrdev_lock);
	major = dynamic_major();
	ret = major ? register_one(major, baseminor, count, name, NULL) : -EBUSY;
	pthread_mutex_unlock(&chrdev_lock);
	if (!ret)
		*dev = MKDEV(major, baseminor);
	return ret;
}

int register_chrdev_region(dev_t from, unsigned int count, const char *name)
{
	uint64_t first = (uint64_t)from;
	uint64_t end = first + count;
	uint64_t n;
	int ret = 0;

	if (!name || !count || !MAJOR(from) ||
	    end > ((uint64_t)LINUXU_CHRDEV_MAX_MAJOR << MINORBITS))
		return -EINVAL;
	pthread_mutex_lock(&chrdev_lock);
	for (n = first; n < end;) {
		unsigned int major = (unsigned int)(n >> MINORBITS);
		unsigned int minor = (unsigned int)(n & MINORMASK);
		unsigned int part = (unsigned int)((end - n) <
				(MINORMASK + 1U - minor) ? (end - n) :
				(MINORMASK + 1U - minor));
		ret = register_one(major, minor, part, name, NULL);
		if (ret)
			break;
		n += part;
	}
	if (ret) {
		uint64_t rollback_end = n;
		for (n = first; n < rollback_end;) {
			unsigned int major = (unsigned int)(n >> MINORBITS);
			unsigned int minor = (unsigned int)(n & MINORMASK);
			unsigned int part = (unsigned int)((rollback_end - n) <
					(MINORMASK + 1U - minor) ? (rollback_end - n) :
					(MINORMASK + 1U - minor));
			unregister_one(major, minor, part, false);
			n += part;
		}
	}
	pthread_mutex_unlock(&chrdev_lock);
	return ret;
}

void unregister_chrdev_region(dev_t from, unsigned int count)
{
	uint64_t n = (uint64_t)from;
	uint64_t end = n + count;

	if (!count || !MAJOR(from) ||
	    end > ((uint64_t)LINUXU_CHRDEV_MAX_MAJOR << MINORBITS))
		return;
	pthread_mutex_lock(&chrdev_lock);
	while (n < end) {
		unsigned int major = (unsigned int)(n >> MINORBITS);
		unsigned int minor = (unsigned int)(n & MINORMASK);
		unsigned int part = (unsigned int)((end - n) <
				(MINORMASK + 1U - minor) ? (end - n) :
				(MINORMASK + 1U - minor));
		unregister_one(major, minor, part, false);
		n += part;
	}
	pthread_mutex_unlock(&chrdev_lock);
}

int linuxu_chrdev_lookup(dev_t dev,
			const struct file_operations **fops)
{
	struct chrdev_registration *r;
	unsigned int major = MAJOR(dev);
	unsigned int minor = MINOR(dev);
	int ret = -ENXIO;

	if (!fops)
		return -EINVAL;
	*fops = NULL;
	pthread_mutex_lock(&chrdev_lock);
	for (r = chrdevs; r; r = r->next)
		if (r->major == major && minor >= r->first_minor &&
		    minor - r->first_minor < r->count && r->fops) {
			*fops = r->fops;
			ret = 0;
			break;
		}
	pthread_mutex_unlock(&chrdev_lock);
	return ret;
}

int linuxu_chrdev_find(const char *name, unsigned int *major)
{
	struct chrdev_registration *r;
	int ret = -ENXIO;

	if (!name || !major)
		return -EINVAL;
	pthread_mutex_lock(&chrdev_lock);
	for (r = chrdevs; r; r = r->next)
		if (r->fops && !strcmp(r->name, name)) {
			*major = r->major;
			ret = 0;
			break;
		}
	pthread_mutex_unlock(&chrdev_lock);
	return ret;
}
