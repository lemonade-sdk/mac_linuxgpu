/* host/fw_mailbox_service.c - user-space firmware servicer (see
 * fw_mailbox_service.h and linuxu/headers/rt/fw_mailbox.h). */
#include "fw_mailbox_service.h"
#include "../linuxu/headers/rt/fw_mailbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct mlg_fw_service {
	struct mlg_fw_mailbox *mailbox;
	uint8_t *data;
	uint32_t capacity;
	char root[PATH_MAX];
	pthread_t thread;
	int thread_started;
	int stop;
	uint64_t served;
	uint64_t missing;
	/* Set by fw_mailbox_iokit.c when it owns the mapping. */
	void (*unmap)(struct mlg_fw_service *);
	uint32_t connection;
	uint64_t map_address;
	uint64_t map_size;
};

/* Exposed to fw_mailbox_iokit.c only. */
void mlg_fw_service_set_unmap(struct mlg_fw_service *service,
			      void (*unmap)(struct mlg_fw_service *),
			      uint32_t connection, uint64_t address, uint64_t size);
void mlg_fw_service_get_mapping(const struct mlg_fw_service *service,
				uint32_t *connection, uint64_t *address, uint64_t *size);

static int name_ok(const char *name)
{
	const char *component = name;

	if (!name[0] || name[0] == '/' || strchr(name, '\\'))
		return 0;
	while (*component) {
		const char *end = strchr(component, '/');
		size_t span = end ? (size_t)(end - component) : strlen(component);

		if (!span || (span == 1 && component[0] == '.') ||
		    (span == 2 && component[0] == '.' && component[1] == '.'))
			return 0;
		if (!end)
			break;
		component = end + 1;
		if (!*component)
			return 0;
	}
	return 1;
}

int mlg_fw_service_attach(void *mapped, size_t size, const char *root,
			  struct mlg_fw_service **out)
{
	struct mlg_fw_mailbox *mailbox = mapped;
	struct mlg_fw_service *service;
	const char *chosen = root;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!mapped || size < sizeof(*mailbox) ||
	    __atomic_load_n(&mailbox->magic, __ATOMIC_ACQUIRE) != MLG_FW_MAILBOX_MAGIC ||
	    mailbox->version != MLG_FW_MAILBOX_VERSION ||
	    mailbox->header_size < sizeof(*mailbox) || mailbox->header_size > size ||
	    !mailbox->data_capacity ||
	    mailbox->data_capacity > size - mailbox->header_size)
		return -EINVAL;
	if (!chosen || !*chosen)
		chosen = getenv(MLG_FW_ROOT_ENV);
	if (!chosen || !*chosen)
		chosen = MLG_FW_DEFAULT_ROOT;
	if (strlen(chosen) >= sizeof(service->root))
		return -ENAMETOOLONG;
	service = calloc(1, sizeof(*service));
	if (!service)
		return -ENOMEM;
	service->mailbox = mailbox;
	service->data = (uint8_t *)mapped + mailbox->header_size;
	service->capacity = mailbox->data_capacity;
	strcpy(service->root, chosen);
	__atomic_fetch_add(&mailbox->servicer_generation, 1, __ATOMIC_ACQ_REL);
	__atomic_fetch_add(&mailbox->servicer_heartbeat, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&mailbox->servicer_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
	*out = service;
	return 0;
}

static void reply(struct mlg_fw_mailbox *mailbox, uint32_t seq, int32_t status,
		  uint64_t total, uint64_t offset, uint32_t length)
{
	mailbox->response_status = status;
	mailbox->response_total_size = total;
	mailbox->response_offset = offset;
	mailbox->response_length = length;
	__atomic_store_n(&mailbox->response_seq, seq, __ATOMIC_RELEASE);
}

int mlg_fw_service_poll(struct mlg_fw_service *service)
{
	struct mlg_fw_mailbox *mailbox;
	char name[MLG_FW_MAILBOX_NAME_MAX];
	char path[PATH_MAX];
	struct stat info;
	uint64_t offset, remaining;
	uint32_t seq, length, done = 0;
	int fd;

	if (!service)
		return -EINVAL;
	mailbox = service->mailbox;
	__atomic_fetch_add(&mailbox->servicer_heartbeat, 1, __ATOMIC_RELEASE);
	/* The dext clears the pid when it believes the servicer died; a live
	 * servicer simply claims the mailbox again. */
	if (!__atomic_load_n(&mailbox->servicer_pid, __ATOMIC_ACQUIRE))
		__atomic_store_n(&mailbox->servicer_pid, (uint32_t)getpid(),
				 __ATOMIC_RELEASE);
	seq = __atomic_load_n(&mailbox->request_seq, __ATOMIC_ACQUIRE);
	if (!seq || seq == __atomic_load_n(&mailbox->response_seq, __ATOMIC_ACQUIRE))
		return 0;

	memcpy(name, mailbox->request_name, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	offset = mailbox->request_offset;
	if (!name_ok(name) ||
	    snprintf(path, sizeof(path), "%s/%s", service->root, name) >= (int)sizeof(path)) {
		reply(mailbox, seq, MLG_FW_STATUS_INVAL, 0, offset, 0);
		return 1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		int missing = errno == ENOENT || errno == ENOTDIR;
		if (missing && offset == 0)
			service->missing++;
		reply(mailbox, seq, missing ? MLG_FW_STATUS_NOENT : MLG_FW_STATUS_IO,
		      0, offset, 0);
		return 1;
	}
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		close(fd);
		reply(mailbox, seq, MLG_FW_STATUS_NOENT, 0, offset, 0);
		return 1;
	}
	if ((uint64_t)info.st_size > MLG_FW_MAILBOX_MAX_FILE) {
		close(fd);
		reply(mailbox, seq, MLG_FW_STATUS_FBIG, (uint64_t)info.st_size, offset, 0);
		return 1;
	}
	if (!info.st_size || offset >= (uint64_t)info.st_size) {
		close(fd);
		reply(mailbox, seq, info.st_size ? MLG_FW_STATUS_INVAL : MLG_FW_STATUS_IO,
		      (uint64_t)info.st_size, offset, 0);
		return 1;
	}
	remaining = (uint64_t)info.st_size - offset;
	length = remaining > service->capacity ? service->capacity : (uint32_t)remaining;
	while (done < length) {
		ssize_t got = pread(fd, service->data + done, length - done,
				    (off_t)(offset + done));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		done += (uint32_t)got;
	}
	close(fd);
	if (done != length) {
		reply(mailbox, seq, MLG_FW_STATUS_IO, (uint64_t)info.st_size, offset, 0);
		return 1;
	}
	if (offset + length == (uint64_t)info.st_size)
		service->served++;
	reply(mailbox, seq, MLG_FW_STATUS_OK, (uint64_t)info.st_size, offset, length);
	return 1;
}

void mlg_fw_service_detach(struct mlg_fw_service *service)
{
	if (!service)
		return;
	__atomic_store_n(&service->mailbox->servicer_pid, 0, __ATOMIC_RELEASE);
	if (service->unmap)
		service->unmap(service);
	free(service);
}

static void *service_thread(void *context)
{
	struct mlg_fw_service *service = context;
	const struct timespec idle = { .tv_sec = 0, .tv_nsec = 1000000L };

	while (!__atomic_load_n(&service->stop, __ATOMIC_ACQUIRE)) {
		if (mlg_fw_service_poll(service) == 0)
			nanosleep(&idle, NULL);
	}
	return NULL;
}

int mlg_fw_service_start(void *mapped, size_t size, const char *root,
			 struct mlg_fw_service **out)
{
	int ret = mlg_fw_service_attach(mapped, size, root, out);

	if (ret)
		return ret;
	if (pthread_create(&(*out)->thread, NULL, service_thread, *out) != 0) {
		mlg_fw_service_detach(*out);
		*out = NULL;
		return -EAGAIN;
	}
	(*out)->thread_started = 1;
	return 0;
}

void mlg_fw_service_stop(struct mlg_fw_service *service)
{
	if (!service)
		return;
	if (service->thread_started) {
		__atomic_store_n(&service->stop, 1, __ATOMIC_RELEASE);
		pthread_join(service->thread, NULL);
	}
	mlg_fw_service_detach(service);
}

uint64_t mlg_fw_service_served(const struct mlg_fw_service *service)
{
	return service ? service->served : 0;
}

uint64_t mlg_fw_service_missing(const struct mlg_fw_service *service)
{
	return service ? service->missing : 0;
}

void mlg_fw_service_set_unmap(struct mlg_fw_service *service,
			      void (*unmap)(struct mlg_fw_service *),
			      uint32_t connection, uint64_t address, uint64_t size)
{
	service->unmap = unmap;
	service->connection = connection;
	service->map_address = address;
	service->map_size = size;
}

void mlg_fw_service_get_mapping(const struct mlg_fw_service *service,
				uint32_t *connection, uint64_t *address, uint64_t *size)
{
	*connection = service->connection;
	*address = service->map_address;
	*size = service->map_size;
}
