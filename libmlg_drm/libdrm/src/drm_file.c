/* libdrm for mac_linuxgpu: DRM files as descriptors of this process
 * (drm_internal.h). */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mlg_drm.h"
#include "xf86drm.h"
#include "drm_internal.h"

struct drm_file {
	ino_t ino;		/* the proxy socket's inode */
	int kept;		/* this library's end */
	int driver_fd;
	int node;	/* DRM_NODE_* + 1 for an opened device node, else 0 */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct drm_file *files;
static size_t nfiles, capfiles;
static int kq = -1;
static pthread_t watcher;

static bool debug(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("LIBDRM_MLG_DEBUG") && *getenv("LIBDRM_MLG_DEBUG") != '0';
	return enabled;
}

static struct drm_file *find_locked(ino_t ino)
{
	for (size_t i = 0; i < nfiles; ++i)
		if (files[i].ino == ino)
			return &files[i];
	return NULL;
}

/* Close the driver file of every proxy whose last copy was closed. */
static void *watch(void *arg)
{
	(void)arg;
	for (;;) {
		struct kevent ev[16];
		int n = kevent(kq, NULL, 0, ev, 16, NULL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return NULL;
		}
		for (int i = 0; i < n; ++i) {
			const int kept = (int)ev[i].ident;
			int driver_fd = -1;

			if (!(ev[i].flags & EV_EOF)) {
				/* Bytes written to a proxy carry nothing. */
				char sink[256];

				while (read(kept, sink, sizeof(sink)) > 0)
					;
				continue;
			}
			pthread_mutex_lock(&lock);
			for (size_t j = 0; j < nfiles; ++j) {
				if (files[j].kept == kept) {
					driver_fd = files[j].driver_fd;
					files[j] = files[--nfiles];
					break;
				}
			}
			pthread_mutex_unlock(&lock);
			if (driver_fd < 0)
				continue;
			/* Closing the descriptor removes its kevent. */
			close(kept);
			if (debug())
				fprintf(stderr, "libdrm-mlg: last copy closed, driver file %d released\n",
					driver_fd);
			(void)mlg_close(driver_fd);
		}
	}
}

static int start_locked(void)
{
	if (kq >= 0)
		return 0;
	kq = kqueue();
	if (kq < 0)
		return -1;
	(void)fcntl(kq, F_SETFD, FD_CLOEXEC);
	if (pthread_create(&watcher, NULL, watch, NULL)) {
		close(kq);
		kq = -1;
		errno = EAGAIN;
		return -1;
	}
	pthread_detach(watcher);
	return 0;
}

int drm_file_wrap(int driver_fd, bool cloexec)
{
	int sv[2] = { -1, -1 }, one = 1, e;
	struct stat st;
	struct kevent ev;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		goto fail;
	(void)setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	(void)setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	(void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(sv[1], F_SETFL, O_NONBLOCK);
	if (cloexec)
		(void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	if (fstat(sv[0], &st))
		goto fail;

	pthread_mutex_lock(&lock);
	if (start_locked())
		goto fail_locked;
	if (nfiles == capfiles) {
		size_t cap = capfiles ? capfiles * 2 : 16;
		struct drm_file *grown = realloc(files, cap * sizeof(*grown));

		if (!grown) {
			errno = ENOMEM;
			goto fail_locked;
		}
		files = grown;
		capfiles = cap;
	}
	EV_SET(&ev, sv[1], EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(kq, &ev, 1, NULL, 0, NULL))
		goto fail_locked;
	files[nfiles++] = (struct drm_file){ .ino = st.st_ino, .kept = sv[1],
					     .driver_fd = driver_fd };
	pthread_mutex_unlock(&lock);
	if (debug())
		fprintf(stderr, "libdrm-mlg: driver file %d is descriptor %d\n", driver_fd, sv[0]);
	return sv[0];

fail_locked:
	pthread_mutex_unlock(&lock);
fail:
	e = errno;
	if (sv[0] >= 0)
		close(sv[0]);
	if (sv[1] >= 0)
		close(sv[1]);
	(void)mlg_close(driver_fd);
	errno = e;
	return -1;
}

static struct drm_file *lookup_locked(int fd)
{
	struct stat st;

	if (fd < 0 || fstat(fd, &st) || !S_ISSOCK(st.st_mode))
		return NULL;
	return find_locked(st.st_ino);
}

int drm_file_driver_fd(int fd)
{
	struct drm_file *f;
	int driver_fd = -1;

	pthread_mutex_lock(&lock);
	f = lookup_locked(fd);
	if (f)
		driver_fd = f->driver_fd;
	pthread_mutex_unlock(&lock);
	if (driver_fd < 0)
		errno = EBADF;
	return driver_fd;
}

int drm_file_node_type(int fd)
{
	struct drm_file *f;
	int type = -1;

	pthread_mutex_lock(&lock);
	f = lookup_locked(fd);
	if (f && f->node)
		type = f->node - 1;
	pthread_mutex_unlock(&lock);
	return type;
}

bool drm_file_is_node(int fd)
{
	return drm_file_node_type(fd) >= 0;
}

void drm_file_set_node(int fd, int type)
{
	struct drm_file *f;

	pthread_mutex_lock(&lock);
	f = lookup_locked(fd);
	if (f)
		f->node = type + 1;
	pthread_mutex_unlock(&lock);
}

int drm_file_any_node(void)
{
	int driver_fd = -1;

	pthread_mutex_lock(&lock);
	for (size_t i = 0; i < nfiles && driver_fd < 0; ++i)
		if (files[i].node == DRM_NODE_RENDER + 1)
			driver_fd = files[i].driver_fd;
	for (size_t i = 0; i < nfiles && driver_fd < 0; ++i)
		if (files[i].node)
			driver_fd = files[i].driver_fd;
	pthread_mutex_unlock(&lock);
	return driver_fd;
}
