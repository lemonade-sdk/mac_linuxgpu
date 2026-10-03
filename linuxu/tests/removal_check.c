/* Surprise removal (rt/removal.h) on the CS fixture's device: the GPU leaves
 * the bus while work is outstanding everywhere.
 *   - a render-node client process (the CS self-test) is parked with a
 *     compute job its queue never runs;
 *   - a kernel SDMA copy is queued on an engine that never runs it, and a
 *     thread waits on its fence without a timeout (an async wait);
 *   - a second render-node client process still has its file open.
 * rt_removal_begin must bring every wait back promptly with -ECANCELED,
 * let the parked test process and the open client exit, refuse new opens
 * of the render node (-ENODEV), skip hardware access from then on, and
 * stop cleanly. Runs last: the fixture device stays removed. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/dma-fence.h>
#include <linux/ktime.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <rt/cs_selftest.h>
#include <rt/lx_abi.h>
#include <rt/lx_files.h>
#include <rt/removal.h>

#include "amdgpu.h"
#include "amdgpu_ttm.h"
#include "cs_fixture.h"

extern int usleep(unsigned int usec);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

struct waiter {
	struct dma_fence *fence;
	volatile int done;
	long result;
};

static void *wait_forever(void *arg)
{
	struct waiter *w = arg;

	w->result = dma_fence_wait(w->fence, false);
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

void removal_check(struct pci_dev *pdev)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	struct rt_cs_selftest_result res;
	struct rt_lx_client *open_client = NULL, *late = NULL;
	struct amdgpu_bo *staging = NULL;
	uint64_t staging_gpu = 0;
	void *staging_cpu = NULL;
	struct waiter w = {0};
	pthread_t thread;
	ktime_t start;
	int fd, r;

	/* A client with its render node open. */
	CHECK(!rt_lx_client_create(pdev, 0, "removal-client", &open_client));
	fd = rt_lx_open(open_client, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC);
	CHECK(fd >= 0);

	/* A test process kept with a compute job that never runs. */
	cs_fixture_hold_compute(1);
	r = rt_cs_selftest_run(pdev, &res);
	CHECK(r == -62 && res.status[RT_CS_STEP_TEARDOWN] == RT_CS_PARKED);
	CHECK(rt_cs_selftest_parked() == 1);

	/* A kernel copy on an engine that never runs it, waited for. */
	CHECK(!amdgpu_bo_create_kernel(adev, 1 << 20, PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				       &staging, &staging_gpu, &staging_cpu));
	cs_fixture_hold_sdma(1);
	mutex_lock(&adev->mman.default_entity.lock);
	r = amdgpu_copy_buffer(adev, &adev->mman.default_entity, staging_gpu,
			       staging_gpu + (512 << 10), 4096, NULL, &w.fence, false, 0);
	mutex_unlock(&adev->mman.default_entity.lock);
	CHECK(!r && w.fence);
	CHECK(!pthread_create(&thread, NULL, wait_forever, &w));
	usleep(50000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) && !dma_fence_is_signaled(w.fence));

	/* The device leaves the bus. */
	start = ktime_get();
	CHECK(!rt_removal_begin(pdev));
	CHECK(pci_dev_is_disconnected(pdev) && !pci_device_is_present(pdev));
	CHECK(rt_removal_active(adev) && adev->no_hw_access);
	CHECK(drm_dev_is_unplugged(adev_to_drm(adev)));
	/* Every wait comes back, the work cancelled. */
	for (int i = 0; i < 200 && !__atomic_load_n(&w.done, __ATOMIC_ACQUIRE); ++i)
		usleep(5000);
	CHECK(__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) && w.result == 0);
	CHECK(dma_fence_get_status(w.fence) == -ECANCELED);
	dma_fence_put(w.fence);
	CHECK(!pthread_join(thread, NULL));
	/* The parked test's job completed with it: its process exits. */
	for (int i = 0; i < 200 && rt_cs_selftest_reap(); ++i)
		usleep(5000);
	CHECK(rt_cs_selftest_parked() == 0);
	/* The open client exits; a new one cannot open the render node. */
	rt_lx_client_destroy(open_client);
	CHECK(!rt_lx_client_create(pdev, 0, "late-client", &late));
	CHECK(rt_lx_open(late, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC) < 0);
	rt_lx_client_destroy(late);
	/* Kernel buffers go as well. */
	amdgpu_bo_free_kernel(&staging, &staging_gpu, &staging_cpu);
	CHECK(rt_removal_completions() > 0);
	rt_removal_end();
	CHECK(ktime_ms_delta(ktime_get(), start) < 5000);
	printf("PASS surprise removal offline: waits on held compute and SDMA work return "
	       "with -ECANCELED, a parked test process and an open client exit, the render "
	       "node refuses new opens, no hardware access afterwards (%lu forced completions, "
	       "%lld ms)\n", rt_removal_completions(),
	       (long long)ktime_ms_delta(ktime_get(), start));
}
