#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/upstream-dma-pool.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import sys
out = Path(sys.argv[1])
def function(path, signature):
    source = Path(path).read_text()
    assert source.count(signature) == 1
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    return source[start:end]
functions = [
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c', 'static int amdgpu_gart_dummy_page_init('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c', 'void amdgpu_gart_dummy_page_fini('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static struct page *ttm_pool_alloc_page('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void __free_pages_gpu_account('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void ttm_pool_free_page('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void ttm_pool_type_give('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static enum lru_status take_one_from_lru('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static struct page *ttm_pool_type_take('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void ttm_pool_type_init('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static enum lru_status pool_move_to_dispose_list('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void ttm_pool_dispose_list('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static void ttm_pool_type_fini('),
]
out.joinpath('upstream-dma-pool.inc').write_text('\n'.join(function(*item) for item in functions))
PY
common=(-w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h
  -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections -Ilinuxu/headers)
clang "${common[@]}" -c linuxu/src/rcu.c -o "$work/rcu.o"
clang -w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DLINUXU_DEXT_DK=1 -ffunction-sections -fdata-sections \
  -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/ttm -I"$work" \
  linuxu/tests/test_upstream_dma_pool.c linuxu/src/dart/dart.c \
  linuxu/src/mm/list_lru.c linuxu/src/kmem/{kmemalloc,kmemcheck,vmalloc}.c \
  linuxu/src/sync.c "$work/rcu.o" -Wl,-dead_strip -lpthread -o "$work/test_upstream_dma_pool"
"$work/test_upstream_dma_pool"
