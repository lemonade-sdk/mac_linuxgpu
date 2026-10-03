#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -Ilinuxu/headers linuxu/tests/test_bitmap_contracts.c linuxu/src/shims/bitmap.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_bitmap_contracts"
"$test_dir/test_bitmap_contracts"
# Include order must not change allocation/address alignment contracts.
for first in kernel align; do
  if [ "$first" = kernel ]; then second=align; else second=kernel; fi
  cat > "$test_dir/align.c" <<SRC
#include <linux/$first.h>
#include <linux/$second.h>
_Static_assert(ALIGN(17ul,16) == 32, "round up");
_Static_assert(ALIGN_DOWN(16ul,16) == 16, "aligned down");
_Static_assert(ALIGN_DOWN(17ul,16) == 16, "round down");
int main(void) { return 0; }
SRC
  clang -w -std=gnu11 -Ilinuxu/headers "$test_dir/align.c" -o "$test_dir/align"
  "$test_dir/align"
done
