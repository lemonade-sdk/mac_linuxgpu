/* Production owner namespaces and shared BO lifetime with simulated backing. */
#define main production_fixture_main
#include "test_dext_compute_production.c"
#undef main

int main(int argc, char **argv)
{
    assert(argc == 2);
    dext_compute_select_client(1);
    assert(!dext_compute_start(&pdev));
    uint64_t shared = alloc_bo(DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM), token[3], info[5];
    assert(!dext_compute_bo_export(shared, 11, 22, token));
    assert(token[0] == 11 && token[1] == 22 && token[2] == 4096);
    assert(!dext_compute_bo_export(shared, 33, 44, token));
    assert(token[0] == 11 && token[1] == 22);
    uint64_t ring, metadata, q = create_queue(&ring, &metadata), map[2], status;
    assert(dext_compute_bo_export(ring, 3, 4, info) == -EINVAL_L);
    assert(!dext_compute_bo_map(ring, map));
    void *cpu; uint64_t bytes;
    assert(!dext_compute_bo_memory(map[0], &cpu, &bytes) && cpu && bytes == 4096);
    dext_compute_select_client(2);
    assert(dext_compute_bo_get_info(shared, info) == -ENOENT_L);
    assert(dext_compute_bo_free(shared) == -ENOENT_L);
    assert(dext_compute_bo_map(ring, info) == -ENOENT_L);
    assert(dext_compute_bo_memory(map[0], &cpu, &bytes) == -ENOENT_L && !cpu && !bytes);
    assert(dext_compute_aql_queue_destroy(q, &status) == -ENOENT_L);
    assert(dext_compute_aql_queue_kick(q, 0, &status) == -ENOENT_L);
    assert(dext_compute_bo_export(shared, 1, 2, info) == -EINVAL_L);
    assert(dext_compute_bo_import(0, 0, 4096, info) == -EINVAL_L);
    assert(dext_compute_bo_import(11, 22, 8192, info) == -EINVAL_L);
    assert(dext_compute_bo_import(22, 11, 4096, info) == -ENOENT_L);
    assert(!dext_compute_bo_import(11, 22, 4096, info));
    uint64_t imported = info[0];
    assert(imported != shared && info[2] == 4096 && !frees);
    assert(!dext_compute_bo_get_info(imported, info));
    if (!strcmp(argv[1], "failed-detach")) {
        destroy_error = -ETIMEDOUT;
        assert(dext_compute_release_client(1) == -EBUSY_L);
        assert(!frees && queue.live && context.live);
        assert(dext_compute_bo_free(imported) == -EBUSY_L);
        assert(dext_compute_stop() == -EBUSY_L);
        return 0;
    }
    if (!strcmp(argv[1], "exhaustion")) {
        unsigned imports = 1;
        while (!dext_compute_bo_import(11, 22, 4096, info)) ++imports;
        assert(imports == 8189 && !frees);
        assert(dext_compute_bo_import(11, 22, 4096, info) == -ENOMEM_L);
        assert(!dext_compute_release_client(2) && !frees);
        assert(!dext_compute_stop() && frees == 3 && closes == 1);
        return 0;
    }
    uint64_t peer = alloc_bo(DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM);
    assert(dext_compute_bo_export(peer, 11, 22, info) == -EINVAL_L);
    assert(!dext_compute_release_client(1));
    assert(destroys == 1 && frees == 2 && context.live && !queue.live);
    assert(!dext_compute_bo_get_info(imported, info));
    assert(!dext_compute_bo_export(imported, 99, 100, token));
    assert(token[0] == 11 && token[1] == 22);
    dext_compute_select_client(3);
    assert(!dext_compute_bo_import(11, 22, 4096, info));
    uint64_t third = info[0];
    assert(!dext_compute_release_client(2) && frees == 3);
    assert(!dext_compute_bo_get_info(third, info));
    if (!strcmp(argv[1], "failed-final-free")) {
        free_error = -EIO;
        assert(dext_compute_bo_free(third) == -EBUSY_L);
        assert(frees == 4 && context.live);
        assert(dext_compute_release_client(3) == -EBUSY_L);
        assert(dext_compute_stop() == -EBUSY_L);
        return 0;
    }
    assert(!strcmp(argv[1], "lifetime"));
    assert(!dext_compute_bo_free(third) && frees == 4);
    assert(dext_compute_bo_import(11, 22, 4096, info) == -ENOENT_L);
    uint64_t fresh = alloc_bo(DEXT_COMPUTE_BO_DOMAIN_GTT), fresh_map[2];
    assert(!dext_compute_bo_map(fresh, fresh_map));
    assert(fresh_map[0] != map[0]);
    assert(dext_compute_bo_memory(map[0], &cpu, &bytes) == -ENOENT_L);
    assert(!dext_compute_release_client(3) && frees == 5);
    assert(!dext_compute_stop() && closes == 1);
    puts("Native IPC: client isolation, token collision, exporter exit, final release and stale maps passed");
}
