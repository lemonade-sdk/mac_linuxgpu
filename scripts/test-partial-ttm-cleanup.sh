#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
def function(path, signature):
    source = Path(path).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    for end in range(opening + 1, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1] + '\n'
    raise RuntimeError(signature)
functions = [
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_sys_manager.c', 'void ttm_sys_man_init('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'static void ttm_global_release('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'static int ttm_global_init('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'int ttm_device_init('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'void ttm_device_fini('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'static void ttm_bo_cleanup_memtype_use('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'static int ttm_bo_individualize_resv('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'static void ttm_bo_flush_all_fences('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'static void ttm_bo_delayed_delete('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'static void ttm_bo_release('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'void ttm_bo_put('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'void ttm_bo_fini('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'void ttm_bo_move_to_lru_tail('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'int ttm_bo_evict_first('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'void ttm_bo_pin('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo.c', 'void ttm_bo_unpin('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo_util.c', 'void ttm_mem_io_free('),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_bo_util.c', 'void ttm_bo_kunmap('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'static void amdgpu_bo_destroy('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'static void amdgpu_bo_user_destroy('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'bool amdgpu_bo_is_amdgpu_bo('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_kunmap('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_unref('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_unpin('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_free_kernel('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_move_notify('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.c', 'void amdgpu_bo_release_notify('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'static void\namdgpu_bo_delete_mem_notify('),
    ('third_party/linux/drivers/gpu/drm/drm_gem.c', 'void\ndrm_gem_object_free('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_hmm.c', 'void amdgpu_hmm_unregister('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c', 'static void amdgpu_gem_object_free('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gtt_mgr.c', 'int amdgpu_gtt_mgr_init('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gtt_mgr.c', 'void amdgpu_gtt_mgr_fini('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_preempt_mgr.c', 'int amdgpu_preempt_mgr_init('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_preempt_mgr.c', 'void amdgpu_preempt_mgr_fini('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'static int amdgpu_ttm_init_on_chip('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'int amdgpu_ttm_mark_vram_reserved('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'static int amdgpu_ttm_alloc_vram_resv_regions('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell_mgr.c', 'int amdgpu_doorbell_create_kernel_doorbells('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'static int amdgpu_ttm_pools_init('),
    ('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c', 'int amdgpu_ttm_init('),
]
out.joinpath('upstream_partial_ttm.inc').write_text(''.join(function(*entry) for entry in functions))
config = Path('mk/host_clang.mk').read_text()
paths = re.search(r'INCPATHS := \\\n(.*?)(?:\n\n)', config, re.S)
out.joinpath('includes').write_text('\n'.join(re.findall(r'-I[^\s\\]+', paths[1])))
PY
includes=()
while IFS= read -r path || [[ -n "$path" ]]; do includes+=("$path"); done < "$work/includes"
mkdir -p "$work/include/DriverKit" "$work/include/PCIDriverKit"
for header in IOService IOBufferMemoryDescriptor IODMACommand IOMemoryMap IOLib; do
  printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/DriverKit/$header.h"
done
printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/PCIDriverKit/IOPCIDevice.h"
common=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections)
objects=()
for source in linuxu/src/amdgpu-rt/ttm_cleanup.c linuxu/src/mm/page.c \
  linuxu/src/dart/{dart,dma_mask}.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  linuxu/src/shims/{task,kthread,printk,dma_fence,timekeeping}.c third_party/linux/lib/rbtree.c \
  linuxu/src/drm/dma_resv.c third_party/linux/drivers/gpu/buddy.c third_party/linux/drivers/gpu/drm/drm_mm.c \
  third_party/linux/drivers/gpu/drm/ttm/{ttm_resource,ttm_range_manager}.c third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c linuxu/src/{sync,bug,spinlock,delay,rcu}.c linuxu/src/sync/ww_mutex.c \
  linuxu/tests/test_partial_ttm_cleanup.c linuxu/tests/iosysctl_host.c; do
  flags=()
  case "$source" in
    */mm/page.c|*/dart/dart.c) flags=(-DLINUXU_DEXT_DK=1) ;;
    */amdgpu_vram_mgr.c) flags=(-Dgpu_buddy_init=partial_gpu_buddy_init) ;;
    */kmem/kmemalloc.c) flags=(-Dmalloc=partial_malloc) ;;
  esac
  object="$work/$(basename "$source" .c).o"
  clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h \
    "${common[@]}" "${flags[@]}" "${includes[@]}" -I"$work" -c "$source" -o "$object"
  objects+=("$object")
done
clang++ -x objective-c++ -std=c++17 -fno-exceptions -fno-objc-exceptions "${common[@]}" \
  -DLINUXU_DEXT=1 -I"$work/include" -Ilinuxu/tests -idirafter linuxu/headers \
  -c linuxu/tests/partial_ttm_driverkit_backend.cpp -o "$work/backend.o"
clang++ "${common[@]}" "${objects[@]}" "$work/backend.o" -Wl,-dead_strip \
  -lpthread -o "$work/test_partial_ttm_cleanup"
"$work/test_partial_ttm_cleanup"
