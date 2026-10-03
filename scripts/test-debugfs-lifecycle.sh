#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections -Ilinuxu/headers -Ilinuxu/src
  -Ilinuxu/tests)
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
  -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
  -Dfree=dext_test_free -Dstrdup=dext_test_strdup
  -Dstrndup=dext_test_strndup)
# Rename the production exported libc adapters only in these objects. The
# checked IOLib backend and sanitizer retain their normal host libc heap.
clang "${flags[@]}" "${heap[@]}" -DLINUXU_DEXT_DK \
  -c linuxu/src/shims/dext_alloc.c -o "$test_dir/heap.o"
clang "${flags[@]}" -c linuxu/tests/dext_heap_backend.c -o "$test_dir/backend.o"
clang "${flags[@]}" -c linuxu/tests/test_debugfs_lifecycle.c -o "$test_dir/test.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/fw/fw_table.c -o "$test_dir/fw.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/fw/fw_mailbox.c -o "$test_dir/fw_mailbox.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/shims/firmware.c -o "$test_dir/firmware.o"
clang "${flags[@]}" -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/kmem/kmemalloc.c -o "$test_dir/kmemalloc.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/kmem/kmemcheck.c -o "$test_dir/kmemcheck.o"
clang "${flags[@]}" "${heap[@]}" -c linuxu/src/shims/debugfs.c -o "$test_dir/debugfs.o"
clang "${flags[@]}" "$test_dir/test.o" "$test_dir/debugfs.o" \
  "$test_dir/fw.o" "$test_dir/fw_mailbox.o" "$test_dir/firmware.o" "$test_dir/kmemalloc.o" \
  "$test_dir/kmemcheck.o" "$test_dir/heap.o" "$test_dir/backend.o" "$test_dir/printk.o" \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_debugfs_lifecycle"
"$test_dir/test_debugfs_lifecycle"

# Reproduce build 212's binding: imported strdup, local DriverKit free.
# This is a host process with simulated allocations and no driver access.
clang "${flags[@]}" "${heap[@]}" -Ustrdup \
  -Dstrdup=dext_heap_test_foreign_strdup \
  -c linuxu/src/shims/debugfs.c -o "$test_dir/debugfs-foreign.o"
clang "${flags[@]}" "$test_dir/test.o" "$test_dir/debugfs-foreign.o" \
  "$test_dir/fw.o" "$test_dir/fw_mailbox.o" "$test_dir/firmware.o" "$test_dir/kmemalloc.o" \
  "$test_dir/kmemcheck.o" "$test_dir/heap.o" "$test_dir/backend.o" "$test_dir/printk.o" \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_foreign_strdup"
if ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0 \
  "$test_dir/test_foreign_strdup" --remove-only >"$test_dir/negative.log" 2>&1; then
  echo "FAIL: foreign strdup was accepted by the DriverKit heap" >&2
  exit 1
fi
if ! rg -q 'AddressSanitizer: heap-buffer-overflow' "$test_dir/negative.log"; then
  cat "$test_dir/negative.log" >&2
  exit 1
fi
echo "Build 212 foreign-strdup cleanup fault reproduced offline; matching heap regression passed"
