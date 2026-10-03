#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections
  -Ilinuxu/headers -Ilinuxu/tests)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
  -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
  -Dfree=dext_test_free -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
for mode in native driverkit; do
  aliases=()
  extra=()
  definitions=()
  if [[ "$mode" == driverkit ]]; then
    aliases=("${heap[@]}")
    definitions=(-DTEST_DRIVERKIT_HEAP)
    clang "${flags[@]}" "${heap[@]}" -DLINUXU_DEXT_DK \
      -c linuxu/src/shims/dext_alloc.c -o "$test_dir/heap.o"
    clang "${flags[@]}" -c linuxu/tests/dext_heap_backend.c -o "$test_dir/backend.o"
    extra=("$test_dir/heap.o" "$test_dir/backend.o")
  fi
  clang "${flags[@]}" "${aliases[@]}" -Dkmemcheck_untrack=zero_test_untrack \
    -c linuxu/src/kmem/kmemalloc.c -o "$test_dir/kmemalloc.o"
  clang "${flags[@]}" "${aliases[@]}" \
    -c linuxu/src/kmem/kmemcheck.c -o "$test_dir/kmemcheck.o"
  clang "${flags[@]}" "${aliases[@]}" \
    -c linuxu/src/kmem/slab.c -o "$test_dir/slab.o"
  # memdup_user and the copy helpers go through the uaccess backend.
  uaccess=()
  for src in linuxu/src/mm/uaccess.c linuxu/src/mm/mm.c linuxu/src/shims/task.c \
    linuxu/src/rwsem.c linuxu/src/delay.c; do
    clang "${flags[@]}" "${aliases[@]}" \
      -c "$src" -o "$test_dir/uaccess-$(basename "$src" .c).o"
    uaccess+=("$test_dir/uaccess-$(basename "$src" .c).o")
  done
  clang "${flags[@]}" "${definitions[@]}" linuxu/tests/test_zero_allocations.c \
    "$test_dir/kmemalloc.o" "$test_dir/kmemcheck.o" "$test_dir/slab.o" \
    "${uaccess[@]}" "${extra[@]}" -Wl,-dead_strip -lpthread -o "$test_dir/$mode"
  "$test_dir/$mode"
done
