#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined)
clang "${flags[@]}" -DLINUXU_TEST_DEXT_SYNC \
  -include linuxu/tests/dext_sync_rename.h \
  -c linuxu/src/shims/dext_sync.c -o "$test_dir/dext_sync.o"
clang "${flags[@]}" linuxu/tests/test_dext_sync.c \
  "$test_dir/dext_sync.o" -o "$test_dir/test_dext_sync"
"$test_dir/test_dext_sync"
