#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-std=gnu11 -g -O1 -fsanitize=address,undefined)
clang "${flags[@]}" -DLINUXU_TEST_DEXT_SYNC \
  -include linuxu/tests/dext_sync_rename.h \
  -c linuxu/src/shims/dext_sync.c -o "$test_dir/sync.o"
clang "${flags[@]}" -DLINUXU_TEST_DEXT_THREADS \
  -include linuxu/tests/dext_sync_rename.h \
  -c linuxu/src/shims/dext_threads.c -o "$test_dir/threads.o"
clang "${flags[@]}" -Dmain=linuxu_sync_fixture_unused_main \
  -c linuxu/tests/test_dext_sync.c -o "$test_dir/backend.o"
clang "${flags[@]}" linuxu/tests/test_dext_threads.c \
  "$test_dir/threads.o" "$test_dir/sync.o" "$test_dir/backend.o" \
  -o "$test_dir/test_dext_threads"
"$test_dir/test_dext_threads"
