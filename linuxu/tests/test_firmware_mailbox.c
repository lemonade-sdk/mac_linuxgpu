/* test_firmware_mailbox.c - on-demand firmware through the host servicer.
 *
 * Runs the dext side (linuxu request_firmware + fw_mailbox) against the
 * real user-space servicer (host/fw_mailbox_service.c) over an in-process
 * shared region, with a temporary firmware root laid out like
 * /lib/firmware.  Covers: miss without a servicer, fetch (chunked), cache
 * hit, push-then-request precedence, miss = -ENOENT, disk over embedded
 * fallback, unrepresentable names, request timeout, dead-servicer
 * detection and detach.  No file list belongs to any particular GPU.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/firmware.h>
#include <rt/fw_mailbox.h>

#include "fw/fw_table.h"
#include "fw/fw_mailbox.h"
#include "fw/fw_rodata.h"
#include "fw_mailbox_service.h"

static char root[256];

static void write_file(const char *name, const uint8_t *bytes, size_t size)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, name);
	f = fopen(path, "wb");
	assert(f);
	assert(fwrite(bytes, 1, size, f) == size);
	fclose(f);
}

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* A servicer that is alive (heartbeat advances) but never answers. */
static int stall_stop;
static void *stalled_servicer(void *context)
{
	struct mlg_fw_mailbox *mailbox = context;
	const struct timespec tick = { 0, 1000000L };

	while (!__atomic_load_n(&stall_stop, __ATOMIC_ACQUIRE)) {
		__atomic_fetch_add(&mailbox->servicer_heartbeat, 1, __ATOMIC_RELEASE);
		nanosleep(&tick, NULL);
	}
	return NULL;
}

int main(void)
{
	const size_t region_size = MLG_FW_MAILBOX_HEADER_SIZE + 4096;
	uint8_t *region = aligned_alloc(16384, MLG_FW_MAILBOX_HEADER_SIZE + 16384);
	struct mlg_fw_mailbox *mailbox = (struct mlg_fw_mailbox *)region;
	struct mlg_fw_service *service = NULL;
	const struct firmware *fw = NULL;
	uint8_t big[10000], small[64];
	char dir[300];

	assert(region);
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (uint8_t)(i * 7 + 3);
	memset(small, 0x77, sizeof(small));
	snprintf(root, sizeof(root), "%s/fwroot.XXXXXX",
		 getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	assert(mkdtemp(root));
	snprintf(dir, sizeof(dir), "%s/amdgpu", root);
	assert(mkdir(dir, 0755) == 0);
	write_file("amdgpu/fetched_9_8_7.bin", big, sizeof(big));
	write_file("amdgpu/pushed_9_8_7.bin", small, sizeof(small));

	fw_table_clear();
	fw_mailbox_set_timeouts(2000, 1000);

	/* 1. no mailbox: a miss is -ENOENT immediately, *fw stays NULL */
	fw = (const struct firmware *)0x1;
	assert(request_firmware(&fw, "amdgpu/fetched_9_8_7.bin", NULL) == -ENOENT);
	assert(fw == NULL);

	/* 2. mailbox without a servicer: still an immediate miss */
	assert(fw_mailbox_attach(region, region_size) == 0);
	assert(mailbox->magic == MLG_FW_MAILBOX_MAGIC);
	assert(mailbox->data_capacity == 4096);
	assert(!fw_mailbox_servicer_present());
	uint64_t t0 = now_ms();
	assert(request_firmware(&fw, "amdgpu/fetched_9_8_7.bin", NULL) == -ENOENT);
	assert(now_ms() - t0 < 500);

	/* 3. servicer attached: fetched in three 4 KiB chunks, byte exact */
	assert(mlg_fw_service_start(region, region_size, root, &service) == 0);
	assert(fw_mailbox_servicer_present());
	assert(request_firmware(&fw, "amdgpu/fetched_9_8_7.bin", NULL) == 0);
	assert(fw && fw->size == sizeof(big) && !memcmp(fw->data, big, sizeof(big)));
	release_firmware(fw);
	assert(mlg_fw_service_served(service) == 1);
	assert(fw_table_has_override("amdgpu/fetched_9_8_7.bin"));

	/* 4. cache hit: a second request does not ask the servicer again */
	assert(request_firmware(&fw, "amdgpu/fetched_9_8_7.bin", NULL) == 0);
	assert(fw->size == sizeof(big));
	release_firmware(fw);
	assert(mlg_fw_service_served(service) == 1);

	/* 5. push then request: the pushed bytes win over the disk file */
	uint8_t pushed[48];
	memset(pushed, 0x31, sizeof(pushed));
	assert(fw_table_override("amdgpu/pushed_9_8_7.bin", pushed, sizeof(pushed)) == 0);
	assert(request_firmware(&fw, "amdgpu/pushed_9_8_7.bin", NULL) == 0);
	assert(fw->size == sizeof(pushed) && fw->data[0] == 0x31);
	release_firmware(fw);
	assert(mlg_fw_service_served(service) == 1);

	/* 6. miss: absent on disk and not embedded -> -ENOENT, like Linux */
	fw = (const struct firmware *)0x1;
	assert(request_firmware(&fw, "amdgpu/absent_9_8_7.bin", NULL) == -ENOENT);
	assert(fw == NULL);
	assert(mlg_fw_service_missing(service) == 1);
	/* the optional-firmware entry point behaves the same */
	assert(firmware_request_nowarn(&fw, "amdgpu/absent_9_8_7_kicker.bin", NULL) == -ENOENT);
	assert(mlg_fw_service_missing(service) == 2);

	/* 7. embedded fallback vs disk (only when the build embedded files) */
	size_t count = 0;
	const struct fw_rodata_entry *rows = fw_rodata_entries(&count);
	if (count) {
		const char *name = rows[0].name;
		assert(fw_table_register_embedded() == 0);
		/* not on disk: the servicer misses, the embedded row answers */
		assert(request_firmware(&fw, name, NULL) == 0);
		assert(fw->size == rows[0].size);
		release_firmware(fw);
		/* on disk: the installed file wins over the embedded row */
		write_file(name, small, sizeof(small));
		assert(request_firmware(&fw, name, NULL) == 0);
		assert(fw->size == sizeof(small) && fw->data[0] == 0x77);
		release_firmware(fw);
	}

	/* 8. names that could escape the firmware root are never served */
	assert(request_firmware(&fw, "../amdgpu/fetched_9_8_7.bin", NULL) == -ENOENT);
	assert(request_firmware(&fw, "/etc/hosts", NULL) == -ENOENT);
	assert(request_firmware(&fw, "amdgpu/../amdgpu/fetched_9_8_7.bin", NULL) == -ENOENT);

	/* 9. detach: later misses are immediate again */
	mlg_fw_service_stop(service);
	service = NULL;
	assert(!fw_mailbox_servicer_present());
	t0 = now_ms();
	assert(request_firmware(&fw, "amdgpu/another_9_8_7.bin", NULL) == -ENOENT);
	assert(now_ms() - t0 < 500);

	/* 10. timeout: a live servicer that never answers -> -ENOENT after the
	 * request timeout, not a hang */
	pthread_t staller;
	fw_mailbox_set_timeouts(300, 5000);
	__atomic_store_n(&mailbox->servicer_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
	stall_stop = 0;
	assert(pthread_create(&staller, NULL, stalled_servicer, mailbox) == 0);
	t0 = now_ms();
	assert(request_firmware(&fw, "amdgpu/slow_9_8_7.bin", NULL) == -ENOENT);
	uint64_t waited = now_ms() - t0;
	assert(waited >= 300 && waited < 3000);
	__atomic_store_n(&stall_stop, 1, __ATOMIC_RELEASE);
	pthread_join(staller, NULL);

	/* 11. a dead servicer (pid set, heartbeat stalled) is detected after
	 * the liveness window and marked detached, so the next miss is fast */
	fw_mailbox_set_timeouts(5000, 200);
	__atomic_store_n(&mailbox->servicer_pid, 12345u, __ATOMIC_RELEASE);
	t0 = now_ms();
	assert(request_firmware(&fw, "amdgpu/dead_9_8_7.bin", NULL) == -ENOENT);
	waited = now_ms() - t0;
	assert(waited >= 200 && waited < 3000);
	assert(mailbox->servicer_pid == 0);
	t0 = now_ms();
	assert(request_firmware(&fw, "amdgpu/dead_9_8_7.bin", NULL) == -ENOENT);
	assert(now_ms() - t0 < 100);

	/* 12. a servicer restarted on the same region resumes service and
	 * answers a request left outstanding by a previous one */
	fw_mailbox_set_timeouts(0, 0);
	write_file("amdgpu/late_9_8_7.bin", small, sizeof(small));
	assert(mlg_fw_service_start(region, region_size, root, &service) == 0);
	assert(request_firmware(&fw, "amdgpu/late_9_8_7.bin", NULL) == 0);
	assert(fw->size == sizeof(small));
	release_firmware(fw);
	mlg_fw_service_stop(service);

	fw_mailbox_detach();
	assert(mailbox->magic == 0);
	assert(mlg_fw_service_attach(region, region_size, root, &service) == -EINVAL);
	fw_table_clear();
	free(region);
	puts("firmware mailbox: passed");
	return 0;
}
