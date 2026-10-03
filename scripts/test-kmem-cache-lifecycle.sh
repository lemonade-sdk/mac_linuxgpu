#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -Ilinuxu/headers -Ilinuxu/tests)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
  -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
  -Dfree=dext_test_free -Dstrdup=dext_test_strdup
  -Dstrndup=dext_test_strndup)
clang "${flags[@]}" "${heap[@]}" -DLINUXU_DEXT_DK \
  -c linuxu/src/shims/dext_alloc.c -o "$test_dir/heap.o"
clang "${flags[@]}" -c linuxu/tests/dext_heap_backend.c -o "$test_dir/backend.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/kmem/kmemalloc.c -o "$test_dir/kmemalloc.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/kmem/kmemcheck.c -o "$test_dir/kmemcheck.o"
# memdup_user copies through the uaccess backend (current->mm's VMAs).
uaccess=()
for src in linuxu/src/mm/uaccess.c linuxu/src/mm/mm.c linuxu/src/shims/task.c \
  linuxu/src/rwsem.c linuxu/src/delay.c; do
  clang "${flags[@]}" "${heap[@]}" -ffunction-sections -fdata-sections \
    -c "$src" -o "$test_dir/uaccess-$(basename "$src" .c).o"
  uaccess+=("$test_dir/uaccess-$(basename "$src" .c).o")
done
clang "${flags[@]}" linuxu/tests/test_kmem_cache_lifecycle.c \
  "$test_dir/heap.o" "$test_dir/backend.o" "$test_dir/kmemalloc.o" \
  "$test_dir/kmemcheck.o" "${uaccess[@]}" -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_kmem_cache_lifecycle"
"$test_dir/test_kmem_cache_lifecycle"
clang "${flags[@]}" "${heap[@]}" -c linuxu/tests/test_dext_heap_alignment.c \
  -o "$test_dir/alignment.o"
clang "${flags[@]}" "$test_dir/alignment.o" "$test_dir/heap.o" \
  "$test_dir/backend.o" -lpthread -o "$test_dir/alignment"
"$test_dir/alignment"

# Restore the former minimum alignment in a private copy, keeping the
# production allocation/address bookkeeping and checked backend unchanged.
python3 - linuxu/src/shims/dext_alloc.c "$test_dir/old-alignment.c" <<'PY'
import pathlib
import sys
source = pathlib.Path(sys.argv[1]).read_text()
fixed = "#define DEXT_HEAP_ALIGNMENT (_Alignof(max_align_t) < 16 ? 16 : _Alignof(max_align_t))"
assert source.count(fixed) == 1
pathlib.Path(sys.argv[2]).write_text(source.replace(fixed, "#define DEXT_HEAP_ALIGNMENT 8"))
PY
clang "${flags[@]}" "${heap[@]}" -DLINUXU_DEXT_DK \
  -c "$test_dir/old-alignment.c" -o "$test_dir/old-heap.o"
clang "${flags[@]}" "$test_dir/alignment.o" "$test_dir/old-heap.o" \
  "$test_dir/backend.o" -lpthread -o "$test_dir/old-alignment"
if UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 ASAN_OPTIONS=abort_on_error=0 \
  "$test_dir/old-alignment" >"$test_dir/negative.log" 2>&1; then
  echo "FAIL: former 8-byte heap alignment was accepted" >&2
  exit 1
fi
if ! rg -q 'runtime error: .*misaligned address.*requires 16 byte alignment' "$test_dir/negative.log"; then
  cat "$test_dir/negative.log" >&2
  exit 1
fi
echo "Former DriverKit heap alignment fault reproduced offline; 16-byte alignment regression passed"
