#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/fatal.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
sources=(linuxu/tests/test_fatal.c linuxu/src/fatal.c linuxu/src/bug.c
  linuxu/src/shims/printk.c linuxu/src/sync.c)
# park: the DriverKit behaviour (record, contain, park the thread forever).
# host: the default for host builds (abort / trap), still relied on by tests.
for mode in park host; do
  flags=()
  if [[ "$mode" == park ]]; then flags+=(-DLINUXU_FATAL_PARK=1); fi
  clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Ilinuxu/headers "${flags[@]}" \
    "${sources[@]}" -Wl,-dead_strip -lpthread -o "$test_dir/test_fatal_$mode"
  if [[ "$mode" == host ]]; then
    # Expected child deaths must not trip sanitizer or crash-report handling.
    ASAN_OPTIONS=handle_abort=0:handle_sigtrap=0:handle_sigill=0 \
      "$test_dir/test_fatal_$mode" 2>"$test_dir/host.err" || {
      cat "$test_dir/host.err"; exit 1; }
  else
    "$test_dir/test_fatal_$mode"
  fi
done
