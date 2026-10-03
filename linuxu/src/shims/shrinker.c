/* Shrinker ownership and registration for pinned TTM; reclaim is host-driven. */
#include <pthread.h>

#include <linux/errno.h>
#include <linux/shrinker.h>

static pthread_mutex_t shrinker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shrinker *shrinkers;

struct shrinker *shrinker_alloc(unsigned int flags, const char *name)
{
	struct shrinker *shrink;
	(void)name;
	shrink = kzalloc(sizeof(*shrink), GFP_KERNEL);
	if (shrink)
		shrink->flags = flags;
	return shrink;
}

int shrinker_register(struct shrinker *shrink)
{
	if (!shrink || !shrink->count_objects || !shrink->scan_objects)
		return -EINVAL;
	pthread_mutex_lock(&shrinker_lock);
	if (shrink->linuxu_registered) {
		pthread_mutex_unlock(&shrinker_lock);
		return -EALREADY;
	}
	shrink->linuxu_next = shrinkers;
	shrinkers = shrink;
	shrink->linuxu_registered = true;
	pthread_mutex_unlock(&shrinker_lock);
	return 0;
}

void shrinker_unregister(struct shrinker *shrink)
{
	struct shrinker **link;
	if (!shrink)
		return;
	pthread_mutex_lock(&shrinker_lock);
	for (link = &shrinkers; *link; link = &(*link)->linuxu_next)
		if (*link == shrink) {
			*link = shrink->linuxu_next;
			shrink->linuxu_next = NULL;
			shrink->linuxu_registered = false;
			break;
		}
	pthread_mutex_unlock(&shrinker_lock);
}

void shrinker_free(struct shrinker *shrink)
{
	if (!shrink)
		return;
	shrinker_unregister(shrink);
	kfree(shrink);
}
