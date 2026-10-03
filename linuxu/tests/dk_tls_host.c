/* Host model of DriverKit thread-local storage: each key is a pthread key,
 * so values are per OS thread exactly as IOThreadLocalStorageSet/Get are.
 * Lets tests build linuxu sources with LINUXU_DEXT_DK on the host. */
#include <pthread.h>
#include <stdint.h>

int IOThreadLocalStorageKeyCreate(uint64_t *key)
{
	pthread_key_t k;
	int ret = pthread_key_create(&k, NULL);

	if (!ret)
		*key = (uint64_t)k;
	return ret;
}

int IOThreadLocalStorageSet(uint64_t key, const void *value)
{
	return pthread_setspecific((pthread_key_t)key, value);
}

void *IOThreadLocalStorageGet(uint64_t key)
{
	return pthread_getspecific((pthread_key_t)key);
}
