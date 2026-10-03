#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/mm-boundaries.XXXXXX")
trap 'rm -rf "$work"' EXIT
common=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections -Ilinuxu/headers)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc -Drealloc=dext_test_realloc
      -Daligned_alloc=dext_test_aligned_alloc -Dfree=dext_test_free
      -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
clang "${common[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work/backend.o"
clang "${common[@]}" "${heap[@]}" -DLINUXU_DEXT_DK=1 -c linuxu/src/shims/dext_alloc.c -o "$work/heap.o"
# The mm lifetime and notifier code runs on host threads (no DriverKit TLS).
mm_objs=()
for src in linuxu/src/mm/mm.c linuxu/src/mm/mmu_notifier.c linuxu/src/rcu.c \
  linuxu/src/rwsem.c linuxu/src/delay.c linuxu/src/shims/fd.c \
  linuxu/src/shims/task.c linuxu/src/bug.c third_party/linux/lib/rbtree.c; do
  clang "${common[@]}" "${heap[@]}" -c "$src" -o "$work/mm-$(basename "$src" .c).o"
  mm_objs+=("$work/mm-$(basename "$src" .c).o")
done
clang "${common[@]}" "${heap[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_mm_boundaries.c \
  linuxu/src/kmem/vmalloc.c linuxu/src/mm/{hmm,usermem,dev_pagemap}.c \
  linuxu/src/shims/shrinker.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  "${mm_objs[@]}" "$work/heap.o" "$work/backend.o" -Wl,-dead_strip -lpthread \
  -o "$work/test_mm_boundaries"
"$work/test_mm_boundaries"
