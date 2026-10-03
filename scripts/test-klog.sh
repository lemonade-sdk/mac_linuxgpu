#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for mode in host dext; do
  flags=()
  if [[ "$mode" == dext ]]; then flags+=(-DLINUXU_DEXT_DK=1); fi
  clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Ilinuxu/headers "${flags[@]}" \
    linuxu/tests/test_klog.c linuxu/src/shims/printk.c linuxu/src/sync.c \
    -Wl,-dead_strip -lpthread -o "$test_dir/test_klog_$mode"
  "$test_dir/test_klog_$mode" "$test_dir/sink_$mode.log" || {
    cat "$test_dir/sink_$mode.log"
    exit 1
  }
done
