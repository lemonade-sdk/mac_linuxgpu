#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/linuxgpu-cleanup.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
common=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
        -ffunction-sections -fdata-sections -Ilinuxu/headers)
heap_aliases=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
              -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
              -Dfree=dext_test_free -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
clang "${common[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work_dir/heap_backend.o"
clang "${common[@]}" "${heap_aliases[@]}" -DLINUXU_DEXT_DK=1 \
  -c linuxu/src/shims/dext_alloc.c -o "$work_dir/dext_alloc.o"
objects=("$work_dir/heap_backend.o" "$work_dir/dext_alloc.o")
for source in linuxu/src/kmem/{kobject,slab,kmemalloc,kmemcheck}.c \
              linuxu/src/shims/{platform_device,sysfs}.c linuxu/src/{sync,sysfs}.c; do
  object="$work_dir/${source//\//_}.o"
  clang "${common[@]}" "${heap_aliases[@]}" -c "$source" -o "$object"
  objects+=("$object")
done
mkdir -p build/tests
clang "${common[@]}" "${heap_aliases[@]}" linuxu/tests/test_probe_cleanup.c \
  "${objects[@]}" -Wl,-dead_strip -lpthread -o build/tests/test_probe_cleanup
UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 build/tests/test_probe_cleanup
