/* linuxu tests: test_dext_compute — the compute selector seam (T-dext-userclient-seam).
 *
 * Exercises the dext_compute.c C API (the functions the ExternalMethod
 * selector bodies route to) on the host, with the gpu-op hook NULL (the
 * in-memory state machine — the host-verifiable scope).  Proves the compute
 * seam's logic is correct: the build number, the QueryInfo tags, the BO
 * registry, the AQL queue registry, the CS/fence path.  The real GPU routing
 * (the dext_compute_dk.mm gpu-op hook) is the p3-hw-gate.
 *
 * The ObjC++ ExternalMethod wrapper itself is dext-only (needs DriverKit
 * headers); this test exercises the C API it routes to, which is the
 * host-verifiable contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dext_compute.h"

static int n_pass, n_fail;
#define CHECK(cond, name) do { \
    if (cond) { n_pass++; printf("  ok: %s\n", name); } \
    else      { n_fail++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

static void test_runtime_build(void)
{
    printf("test_runtime_build\n");
    dext_compute_reset();
    uint64_t out[3] = { 0, 0, 0 };
    int r = dext_compute_runtime_build(out);
    CHECK(r == 0, "runtime_build returns 0");
    CHECK(out[0] == 0x414D444750554142ull, "magic is AMDGPUAB");
    CHECK(out[1] == 1, "ABI is 1");
    CHECK(out[2] == 0, "no unsupported compute feature gates advertised");
}

static void test_query_info_basic(void)
{
    printf("test_query_info_basic\n");
    dext_compute_reset();
    dext_compute_set_gfx_version(12, 0, 1);
    dext_compute_set_vram(0, 32ull * 1024 * 1024 * 1024, 32ull * 1024 * 1024 * 1024);
    dext_compute_set_pci_open(true);

    uint64_t out[32];
    /* tag 1: GFX version (3 values) */
    int r = dext_compute_query_info(1, out, 32);
    CHECK(r == 3, "tag1 returns 3 values");
    CHECK(out[0] == 12 && out[1] == 0 && out[2] == 1, "tag1 = gfx 12.0.1");

    /* tag 2: VRAM sizes (2 values) */
    r = dext_compute_query_info(2, out, 32);
    CHECK(r == 2, "tag2 returns 2 values");
    CHECK(out[0] == 32ull * 1024 * 1024 * 1024, "tag2 visible = 32GB");

    /* tag 3: IP versions (4 values) */
    r = dext_compute_query_info(3, out, 32);
    CHECK(r == 4, "tag3 returns 4 values");

    /* tag 4: bringup reached (1 value) */
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_SDMA_INIT);
    r = dext_compute_query_info(4, out, 32);
    CHECK(r == 1 && out[0] == DEXT_COMPUTE_STAGE_SDMA_INIT, "tag4 = stage");

    /* tag 5: VRAM accounting requires initialized hardware counters */
    r = dext_compute_query_info(5, out, 32);
    CHECK(r == -ENOTREADY_L, "tag5 not ready before hardware init");

    /* tag 8: device spec (32 dwords, header=0 = not resolved) */
    r = dext_compute_query_info(8, out, 32);
    CHECK(r == 32, "tag8 returns 32 values");
    CHECK(out[0] == 0, "tag8 header=0 (not resolved)");

    /* tag 6: needs claimed + valid props → not ready without them */
    r = dext_compute_query_info(6, out, 32);
    CHECK(r == -ENOTREADY_L, "tag6 not ready (no claimed/props)");

    /* with claimed + valid props → 10 values */
    struct dext_compute_props p;
    memset(&p, 0, sizeof(p));
    p.valid = true;
    p.chip_id = 0x74a2;
    p.compute_units = 64;
    p.wavefront_size = 32;
    dext_compute_set_props(&p);
    dext_compute_set_claimed(true);
    r = dext_compute_query_info(6, out, 32);
    CHECK(r == 10, "tag6 returns 10 values when ready");
    CHECK(out[0] == 0x74a2, "tag6 chip_id");
    CHECK(out[4] == 64, "tag6 compute_units");

    /* bad tag */
    r = dext_compute_query_info(99, out, 32);
    CHECK(r == -EINVAL_L, "bad tag → -EINVAL_L");
}

static void test_bo_lifecycle(void)
{
    printf("test_bo_lifecycle\n");
    dext_compute_reset();
    dext_compute_set_pci_open(true);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_SDMA_INIT);

    uint64_t handle = 0, gpu_va = 0, cpu = 0;
    int r = dext_compute_bo_alloc(65536, DEXT_COMPUTE_BO_DOMAIN_GTT, 4096, 0,
                                  &handle, &gpu_va, &cpu);
    CHECK(r == 0, "bo_alloc succeeds");
    CHECK(handle == 1, "first BO handle is 1");

    uint64_t info[5] = { 0 };
    r = dext_compute_bo_get_info(handle, info);
    CHECK(r == 0, "bo_get_info succeeds");
    CHECK(info[2] == 65536, "bo size = 65536");
    CHECK(info[3] == 4096, "bo alignment = 4096");
    CHECK((uint32_t)info[4] == DEXT_COMPUTE_BO_DOMAIN_GTT, "bo domain = GTT");

    uint64_t map_out[2] = { 0 };
    r = dext_compute_bo_map(handle, map_out);
    CHECK(r == 0, "bo_map GTT succeeds");
    CHECK(map_out[0] == 0x10000, "bo_map GTT memory_type = BOBase+0");
    CHECK(map_out[1] == 65536, "bo_map size");

    /* second BO */
    uint64_t handle2 = 0;
    r = dext_compute_bo_alloc(1024, DEXT_COMPUTE_BO_DOMAIN_VRAM, 4096, 0,
                              &handle2, &gpu_va, &cpu);
    CHECK(r == 0 && handle2 == 2, "second BO handle is 2");

    /* free the first */
    r = dext_compute_bo_free(handle);
    CHECK(r == 0, "bo_free succeeds");
    r = dext_compute_bo_get_info(handle, info);
    CHECK(r == -ENOENT_L, "freed BO → -ENOENT_L");

    /* free an unknown handle */
    r = dext_compute_bo_free(999);
    CHECK(r == -ENOENT_L, "free unknown BO → -ENOENT_L");

    /* bad domain */
    r = dext_compute_bo_alloc(1024, 99, 4096, 0, &handle2, &gpu_va, &cpu);
    CHECK(r == -EINVAL_L, "bad domain → -EINVAL_L");

    /* nonzero flags */
    r = dext_compute_bo_alloc(1024, DEXT_COMPUTE_BO_DOMAIN_VRAM, 4096, 1,
                              &handle2, &gpu_va, &cpu);
    CHECK(r == -EINVAL_L, "nonzero flags → -EINVAL_L");
}

static void test_aql_queue_lifecycle(void)
{
    printf("test_aql_queue_lifecycle\n");
    dext_compute_reset();
    dext_compute_set_pci_open(true);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_SDMA_INIT);

    /* need a ring BO (>= packets*64) + a metadata BO (>= 512) */
    uint64_t ring = 0, meta = 0, dummy;
    int r = dext_compute_bo_alloc(64 * 64, DEXT_COMPUTE_BO_DOMAIN_GTT, 4096, 0,
                                  &ring, &dummy, &dummy);
    CHECK(r == 0, "ring BO alloc");
    r = dext_compute_bo_alloc(512, DEXT_COMPUTE_BO_DOMAIN_GTT, 4096, 0,
                              &meta, &dummy, &dummy);
    CHECK(r == 0, "metadata BO alloc");

    uint64_t status = 0, qhandle = 0;
    r = dext_compute_aql_queue_create(ring, meta, 64, &status, &qhandle);
    CHECK(r == 0, "aql_queue_create succeeds");
    CHECK(status == 0, "queue status = 0 (success, no gpu hook)");
    CHECK(qhandle == 1, "queue handle = 1");

    /* kick */
    uint64_t kstatus = 0;
    r = dext_compute_aql_queue_kick(qhandle, 100, &kstatus);
    CHECK(r == 0 && kstatus == 0, "aql_queue_kick succeeds");

    /* service */
    uint64_t sstatus = 0, inactive = 0;
    r = dext_compute_aql_queue_service(qhandle, &sstatus, &inactive);
    CHECK(r == 0, "aql_queue_service succeeds");

    /* can't free a BO a live queue references */
    r = dext_compute_bo_free(ring);
    CHECK(r == -EAGAIN_L, "free ring BO while queue live → -EAGAIN_L");

    /* destroy the queue */
    uint64_t dstatus = 0;
    r = dext_compute_aql_queue_destroy(qhandle, &dstatus);
    CHECK(r == 0, "aql_queue_destroy succeeds");

    /* now the BO can be freed */
    r = dext_compute_bo_free(ring);
    CHECK(r == 0, "ring BO free after queue destroy");

    /* bad packets (not a power of 2) */
    r = dext_compute_aql_queue_create(ring, meta, 100, &status, &qhandle);
    CHECK(r == -EINVAL_L, "non-power-of-2 packets → -EINVAL_L");
    (void)meta;
}

static void test_cs_fence_path(void)
{
    printf("test_cs_fence_path\n");
    dext_compute_reset();
    dext_compute_set_pci_open(true);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_SDMA_INIT);

    uint64_t cs = 0;
    int r = dext_compute_cs_create(DEXT_COMPUTE_CS_IP_SDMA, 0, &cs);
    CHECK(r == 0 && cs == 1, "cs_create SDMA handle=1");

    uint32_t dwords[4] = { 0xDEADBEEF, 0, 0, 0 };
    uint32_t written = 0;
    r = dext_compute_cs_write_dwords(cs, dwords, 4, &written);
    CHECK(r == 0 && written == 4, "cs_write_dwords 4 dwords");

    /* No ring executes SDMA/GFX streams: refuse instead of faking a fence. */
    uint64_t fence = 0;
    r = dext_compute_submit_ib(cs, &fence);
    CHECK(r == -ENOTREADY_L, "submit SDMA CS → -ENOTREADY_L");
    CHECK(fence == 0, "no fence handle without a submission");

    /* nothing was submitted on the CS, so nothing is pending */
    uint64_t wstatus = 99;
    r = dext_compute_wait_fence(cs, 1000000, &wstatus);
    CHECK(r == 0 && wstatus == 0, "wait_fence on idle CS (0)");

    uint64_t csg = 0;
    r = dext_compute_cs_create(DEXT_COMPUTE_CS_IP_GFX, 0, &csg);
    CHECK(r == 0, "cs_create GFX");
    r = dext_compute_cs_write_dwords(csg, dwords, 4, &written);
    CHECK(r == 0, "cs_write GFX");
    r = dext_compute_submit_ib(csg, &fence);
    CHECK(r == -ENOTREADY_L && fence == 0, "submit GFX CS → -ENOTREADY_L");
    CHECK(dext_compute_cs_destroy(csg) == 0, "cs_destroy GFX");

    /* wait on an unknown fence */
    r = dext_compute_wait_fence(5000, 1000000, &wstatus);
    CHECK(r == -ENOENT_L, "wait unknown fence → -ENOENT_L");

    r = dext_compute_cs_destroy(cs);
    CHECK(r == 0, "cs_destroy succeeds");

    /* submit on a destroyed CS */
    r = dext_compute_submit_ib(cs, &fence);
    CHECK(r == -ENOENT_L, "submit destroyed CS → -ENOENT_L");

    /* compute CS → unsupported */
    uint64_t csc = 0;
    r = dext_compute_cs_create(DEXT_COMPUTE_CS_IP_COMPUTE, 0, &csc);
    CHECK(r == 0, "cs_create COMPUTE");
    r = dext_compute_cs_write_dwords(csc, dwords, 4, &written);
    CHECK(r == 0, "cs_write COMPUTE");
    r = dext_compute_submit_ib(csc, &fence);
    CHECK(r == -ENOTREADY_L, "submit COMPUTE CS → -ENOTREADY_L");
}

static void test_aql_dispatch_shape(void)
{
    printf("test_aql_dispatch_shape\n");
    dext_compute_reset();
    dext_compute_set_pci_open(true);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_SDMA_INIT);

    /* AQLDispatchRequest: version(4), flags(4), codeHandle(8@8),
     * descriptorOffset(8@16), kernargHandle(8@24), kernargOffset(8@32),
     * kernargBytes(8@40), ... */
    uint8_t req[208];
    memset(req, 0, sizeof(req));
    uint32_t version = 1;
    memcpy(req + 0, &version, 4);  /* version */
    /* flags at +4 = 0 */
    uint64_t code = 1, kernarg = 2;
    /* need real BOs for code + kernarg */
    uint64_t h1 = 0, h2 = 0, dummy;
    dext_compute_bo_alloc(4096, DEXT_COMPUTE_BO_DOMAIN_VRAM, 4096, 0, &h1, &dummy, &dummy);
    dext_compute_bo_alloc(4096, DEXT_COMPUTE_BO_DOMAIN_VRAM, 4096, 0, &h2, &dummy, &dummy);
    code = h1; kernarg = h2;
    memcpy(req + 8, &code, 8);    /* codeHandle */
    memcpy(req + 16, &code, 8);   /* descriptorOffset=0 */
    uint64_t zero = 0;
    memcpy(req + 16, &zero, 8);
    memcpy(req + 24, &kernarg, 8); /* kernargHandle */
    memcpy(req + 32, &zero, 8);   /* kernargOffset=0 */
    uint64_t kbytes = 16;
    memcpy(req + 40, &kbytes, 8);  /* kernargBytes */
    /* groups[3] at +48, threads[3] at +60 */
    uint32_t groups[3] = { 1, 1, 1 };
    uint32_t threads[3] = { 256, 1, 1 };
    memcpy(req + 48, groups, 12);
    memcpy(req + 60, threads, 12);
    uint32_t timeout = 100;
    memcpy(req + 72, &timeout, 4); /* timeoutUS */

    uint64_t out[5] = { 0 };
    int r = dext_compute_aql_dispatch(req, sizeof(req), out);
    CHECK(r == 0, "aql_dispatch succeeds");
    CHECK(out[0] == 0, "dispatch status = 0");
    CHECK(out[2] == 1, "dispatch stage = 1 (dispatched)");

    /* bad: wrong size */
    r = dext_compute_aql_dispatch(req, 100, out);
    CHECK(r == -EINVAL_L, "aql_dispatch short struct → -EINVAL_L");

    /* bad: not ready (stage != SDMAInit) */
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_NONE);
    r = dext_compute_aql_dispatch(req, sizeof(req), out);
    CHECK(r == -ENOTREADY_L, "aql_dispatch not ready → -ENOTREADY_L");
}

static void test_shutdown_and_notready(void)
{
    printf("test_shutdown_and_notready\n");
    dext_compute_reset();
    /* before pci_open, the not-ready gates hold */
    uint64_t out[32];
    int r = dext_compute_host_window(0, out);
    CHECK(r == -ENOTREADY_L, "host_window not ready (no stage)");
    r = dext_compute_host_mem_test(1024, out);
    CHECK(r == -ENOTREADY_L, "host_mem_test not ready (no pci_open)");

    /* shutdown resets state + reports phase 6 */
    dext_compute_set_pci_open(true);
    uint64_t sout[2] = { 0 };
    r = dext_compute_shutdown(sout);
    CHECK(r == 0, "shutdown succeeds");
    CHECK(sout[1] == 6, "shutdown phase = 6 (complete)");
    /* state is reset: stage is now NONE */
    r = dext_compute_query_info(4, out, 32);
    CHECK(r == 1 && out[0] == DEXT_COMPUTE_STAGE_NONE, "state reset after shutdown");
}

/* Selector 60 must satisfy hsa/abi/amdgpu_atomic_requester.h valid() and
 * report unsupported for enable without claiming an active experiment. */
static int atomic_snapshot_valid(const uint64_t *v)
{
    if (v[0] != 1 || v[5] > 1 || v[6] > 1 || v[7] > UINT32_MAX ||
        (v[6] && !v[5])) return 0;
    for (int i = 1; i <= 4; ++i)
        if (v[i] > UINT16_MAX && v[i] != UINT64_MAX) return 0;
    return 1;
}

static void test_atomic_requester(void)
{
    printf("test_atomic_requester\n");
    uint64_t out[DEXT_COMPUTE_ATOMIC_REQUESTER_WORDS];
    memset(out, 0xa5, sizeof(out));
    int r = dext_compute_atomic_requester(1, out);
    CHECK(r == 0 && atomic_snapshot_valid(out), "enable: valid snapshot");
    CHECK(out[5] == 0 && out[6] == 0, "enable: experiment not active");
    CHECK(out[1] == UINT64_MAX && out[3] == UINT64_MAX, "enable: no config access");
    CHECK(out[7] == DEXT_COMPUTE_ATOMIC_UNSUPPORTED, "enable: status EOPNOTSUPP");
    r = dext_compute_atomic_requester(0, out);
    CHECK(r == 0 && atomic_snapshot_valid(out) && out[7] == 0,
          "disable: nothing to restore");
    CHECK(dext_compute_atomic_requester(2, out) == -EINVAL_L, "enable > 1 rejected");
    CHECK(dext_compute_atomic_requester(1, NULL) == -EINVAL_L, "null output rejected");
}

int main(void)
{
    printf("test_dext_compute (the compute selector seam, host)\n");
    test_runtime_build();
    test_query_info_basic();
    test_bo_lifecycle();
    test_aql_queue_lifecycle();
    test_cs_fence_path();
    test_aql_dispatch_shape();
    test_shutdown_and_notready();
    test_atomic_requester();
    printf("\n== test_dext_compute summary: %d passed, %d failed ==\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
