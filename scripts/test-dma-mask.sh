#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for mode in host dext; do
  flags=()
  if [ "$mode" = dext ]; then
    flags=(-DLINUXU_DEXT_DK=1)
  fi
  clang -w -std=gnu11 -ffunction-sections -fdata-sections \
    "${flags[@]}" -Ilinuxu/headers \
    linuxu/tests/test_dma_mask.c linuxu/src/dart/dma_mask.c \
    -Wl,-dead_strip -o "$test_dir/test_dma_mask_$mode"
  "$test_dir/test_dma_mask_$mode"
done
