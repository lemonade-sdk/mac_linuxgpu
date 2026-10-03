#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections
  -Ilinuxu/headers -Ilinuxu/src -Ilinuxu/tests)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
  -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
  -Dfree=dext_test_free -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
clang "${flags[@]}" "${heap[@]}" -DLINUXU_DEXT_DK \
  -c linuxu/src/shims/dext_alloc.c -o "$test_dir/heap.o"
clang "${flags[@]}" -c linuxu/tests/dext_heap_backend.c -o "$test_dir/backend.o"
for src in linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c \
  linuxu/src/refcount.c linuxu/src/rcu.c linuxu/src/sync.c \
  linuxu/src/shims/fd.c \
  linuxu/src/shims/task.c linuxu/src/shims/kthread.c \
  linuxu/src/bug.c linuxu/src/shims/timekeeping.c \
  linuxu/src/sync/ww_mutex.c linuxu/src/shims/dma_fence.c \
  linuxu/src/shims/dma_fence_chain.c linuxu/src/drm/dma_resv.c; do
  clang "${flags[@]}" "${heap[@]}" -c "$src" -o "$test_dir/$(basename "$src" .c).o"
done
for test in test_fence_lifetime test_fence; do
  clang "${flags[@]}" "linuxu/tests/$test.c" "$test_dir/"*.o \
    -Wl,-dead_strip -lpthread -o "$test_dir/$test"
  "$test_dir/$test"
done

clang "${flags[@]}" "${heap[@]}" -D__KERNEL__ -include linux/autoconf.h \
  -Ithird_party/linux/drivers/gpu/drm -c third_party/linux/drivers/gpu/drm/drm_syncobj.c -o "$test_dir/drm_syncobj.o"
clang "${flags[@]}" linuxu/tests/test_syncobj_lifetime.c "$test_dir/"*.o \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_syncobj_lifetime"
"$test_dir/test_syncobj_lifetime"
