#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/xarray-safety.XXXXXX")
trap 'rm -rf "$work"' EXIT
common=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections -Ilinuxu/headers)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc -Drealloc=dext_test_realloc
      -Daligned_alloc=dext_test_aligned_alloc -Dfree=dext_test_free
      -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
clang "${common[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work/backend.o"
clang "${common[@]}" "${heap[@]}" -DLINUXU_DEXT_DK=1 -c linuxu/src/shims/dext_alloc.c -o "$work/heap.o"
mkdir -p build/tests
clang "${common[@]}" "${heap[@]}" linuxu/tests/test_xarray_safety.c \
  linuxu/src/xarray.c linuxu/src/rcu.c linuxu/src/sync.c "$work/heap.o" "$work/backend.o" \
  -Wl,-dead_strip -lpthread -o build/tests/test_xarray_safety
build/tests/test_xarray_safety
