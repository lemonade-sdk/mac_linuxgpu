/* linuxu shim: fw - dext side of the on-demand firmware mailbox
 * (protocol: <rt/fw_mailbox.h>, API: fw_mailbox.h).
 *
 * request_firmware() runs inside the upstream probe on the dext's serial
 * queue, so no client RPC can be serviced while it waits.  The servicer
 * instead watches shared memory: the dext posts a request, polls for the
 * reply with a short sleep, and gives up after a bounded time.  A servicer
 * that never attached costs nothing; one that attached and then died is
 * detected by its stalled heartbeat and marked detached so later requests
 * fail fast.
 */
#include "fw_mailbox.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/errno.h>
#include <linux/printk.h>
#include <rt/fw_mailbox.h>

#define FW_MAILBOX_DEFAULT_REQUEST_MS  10000u
#define FW_MAILBOX_DEFAULT_LIVENESS_MS 2000u
#define FW_MAILBOX_POLL_NS             1000000L /* 1 ms */

static pthread_mutex_t g_fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mlg_fw_mailbox *g_mailbox;
static uint8_t *g_data;
static uint32_t g_capacity;
static uint32_t g_seq;
static unsigned int g_request_ms = FW_MAILBOX_DEFAULT_REQUEST_MS;
static unsigned int g_liveness_ms = FW_MAILBOX_DEFAULT_LIVENESS_MS;

static uint64_t fw_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void fw_poll_sleep(void)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = FW_MAILBOX_POLL_NS };

	nanosleep(&delay, NULL);
}

int fw_mailbox_attach(void *memory, size_t size)
{
	struct mlg_fw_mailbox *mailbox = memory;

	if (!memory || size < MLG_FW_MAILBOX_HEADER_SIZE + 4096u ||
	    size - MLG_FW_MAILBOX_HEADER_SIZE > UINT32_MAX)
		return -EINVAL;
	pthread_mutex_lock(&g_fetch_lock);
	memset(memory, 0, MLG_FW_MAILBOX_HEADER_SIZE);
	mailbox->version = MLG_FW_MAILBOX_VERSION;
	mailbox->header_size = MLG_FW_MAILBOX_HEADER_SIZE;
	mailbox->data_capacity = (uint32_t)(size - MLG_FW_MAILBOX_HEADER_SIZE);
	/* The magic is published last: a servicer that sees it sees a fully
	 * initialized header. */
	__atomic_store_n(&mailbox->magic, MLG_FW_MAILBOX_MAGIC, __ATOMIC_RELEASE);
	g_mailbox = mailbox;
	g_data = (uint8_t *)memory + MLG_FW_MAILBOX_HEADER_SIZE;
	g_capacity = mailbox->data_capacity;
	g_seq = 0;
	pthread_mutex_unlock(&g_fetch_lock);
	return 0;
}

void fw_mailbox_detach(void)
{
	pthread_mutex_lock(&g_fetch_lock);
	if (g_mailbox)
		__atomic_store_n(&g_mailbox->magic, 0, __ATOMIC_RELEASE);
	g_mailbox = NULL;
	g_data = NULL;
	g_capacity = 0;
	pthread_mutex_unlock(&g_fetch_lock);
}

int fw_mailbox_servicer_present(void)
{
	int present;

	pthread_mutex_lock(&g_fetch_lock);
	present = g_mailbox &&
		__atomic_load_n(&g_mailbox->servicer_pid, __ATOMIC_ACQUIRE) != 0;
	pthread_mutex_unlock(&g_fetch_lock);
	return present;
}

void fw_mailbox_set_timeouts(unsigned int request_ms, unsigned int liveness_ms)
{
	pthread_mutex_lock(&g_fetch_lock);
	g_request_ms = request_ms ? request_ms : FW_MAILBOX_DEFAULT_REQUEST_MS;
	g_liveness_ms = liveness_ms ? liveness_ms : FW_MAILBOX_DEFAULT_LIVENESS_MS;
	pthread_mutex_unlock(&g_fetch_lock);
}

/* Upstream names are relative paths such as "amdgpu/x.bin".  Anything the
 * servicer could resolve outside its firmware root is not representable. */
static int fw_name_ok(const char *name)
{
	size_t length;
	const char *component;

	if (!name)
		return 0;
	length = strnlen(name, MLG_FW_MAILBOX_NAME_MAX);
	if (!length || length >= MLG_FW_MAILBOX_NAME_MAX || name[0] == '/')
		return 0;
	for (component = name; *component;) {
		const char *end = strchr(component, '/');
		size_t span = end ? (size_t)(end - component) : strlen(component);

		if (!span || (span == 2 && component[0] == '.' && component[1] == '.') ||
		    (span == 1 && component[0] == '.'))
			return 0;
		if (!end)
			break;
		component = end + 1;
		if (!*component)
			return 0;
	}
	return strchr(name, '\\') == NULL;
}

/* caller holds g_fetch_lock.  Posts one request and waits for its reply.
 * Returns 0 with the reply visible, or -ENOENT on timeout/servicer loss. */
static int fw_round_trip(const char *name, uint64_t offset)
{
	struct mlg_fw_mailbox *mailbox = g_mailbox;
	uint64_t start = fw_now_ms(), seen_at = start;
	uint64_t heartbeat;
	uint32_t seq;

	if (++g_seq == 0)
		g_seq = 1;
	seq = g_seq;
	/* fw_name_ok() bounded the name below MLG_FW_MAILBOX_NAME_MAX. */
	size_t length = strnlen(name, MLG_FW_MAILBOX_NAME_MAX - 1);
	memcpy(mailbox->request_name, name, length);
	mailbox->request_name[length] = '\0';
	mailbox->request_offset = offset;
	heartbeat = __atomic_load_n(&mailbox->servicer_heartbeat, __ATOMIC_ACQUIRE);
	__atomic_store_n(&mailbox->request_seq, seq, __ATOMIC_RELEASE);

	for (;;) {
		uint64_t now, beat;

		if (__atomic_load_n(&mailbox->response_seq, __ATOMIC_ACQUIRE) == seq)
			return 0;
		now = fw_now_ms();
		beat = __atomic_load_n(&mailbox->servicer_heartbeat, __ATOMIC_ACQUIRE);
		if (beat != heartbeat) {
			heartbeat = beat;
			seen_at = now;
		}
		if (!__atomic_load_n(&mailbox->servicer_pid, __ATOMIC_ACQUIRE)) {
			pr_info("firmware: servicer detached while %s was requested\n",
				name);
			return -ENOENT;
		}
		if (now - seen_at >= g_liveness_ms) {
			/* A dead servicer cannot detach itself.  Mark it gone so
			 * later requests fail without waiting again. */
			__atomic_store_n(&mailbox->servicer_pid, 0, __ATOMIC_RELEASE);
			pr_info("firmware: servicer stopped responding while %s was requested\n",
				name);
			return -ENOENT;
		}
		if (now - start >= g_request_ms) {
			pr_info("firmware: request for %s timed out after %u ms\n",
				name, g_request_ms);
			return -ENOENT;
		}
		fw_poll_sleep();
	}
}

int fw_mailbox_fetch(const char *name, uint8_t **blob, size_t *size)
{
	uint8_t *buffer = NULL;
	uint64_t total = 0, offset = 0;
	int result;

	if (!blob || !size)
		return -EINVAL;
	*blob = NULL;
	*size = 0;
	if (!fw_name_ok(name))
		return -EINVAL;
	pthread_mutex_lock(&g_fetch_lock);
	if (!g_mailbox ||
	    !__atomic_load_n(&g_mailbox->servicer_pid, __ATOMIC_ACQUIRE)) {
		pthread_mutex_unlock(&g_fetch_lock);
		return -ENOENT;
	}
	pr_info("firmware: requesting %s from the host firmware servicer\n", name);
	do {
		int32_t status;
		uint64_t reply_total, reply_offset;
		uint32_t length;

		result = fw_round_trip(name, offset);
		if (result)
			break;
		status = g_mailbox->response_status;
		reply_total = g_mailbox->response_total_size;
		reply_offset = g_mailbox->response_offset;
		length = g_mailbox->response_length;
		if (status == MLG_FW_STATUS_NOENT) {
			result = -ENOENT;
			break;
		}
		if (status == MLG_FW_STATUS_FBIG) {
			result = -EFBIG;
			break;
		}
		if (status != MLG_FW_STATUS_OK) {
			result = status == MLG_FW_STATUS_INVAL ? -EINVAL : -EIO;
			break;
		}
		if (!buffer) {
			if (!reply_total || reply_total > MLG_FW_MAILBOX_MAX_FILE) {
				result = reply_total ? -EFBIG : -EIO;
				break;
			}
			total = reply_total;
			buffer = malloc((size_t)total);
			if (!buffer) {
				result = -ENOMEM;
				break;
			}
		}
		/* The file must not change size between chunks, and each reply
		 * must make progress inside the window it was asked for. */
		if (reply_total != total || reply_offset != offset || !length ||
		    length > g_capacity || length > total - offset) {
			result = -EIO;
			break;
		}
		memcpy(buffer + offset, g_data, length);
		offset += length;
	} while (offset < total);
	pthread_mutex_unlock(&g_fetch_lock);

	if (result) {
		free(buffer);
		if (result == -ENOENT)
			pr_info("firmware: %s not provided by the host\n", name);
		else
			pr_info("firmware: fetching %s failed: %d\n", name, result);
		return result;
	}
	*blob = buffer;
	*size = (size_t)total;
	return 0;
}
