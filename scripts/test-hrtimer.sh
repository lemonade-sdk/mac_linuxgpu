#!/usr/bin/env bash
# hrtimer on the linuxu timer service, under ASan/UBSan.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_hrtimer.c linuxu/src/timer.c linuxu/src/shims/timekeeping.c linuxu/src/bug.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_hrtimer"
"$test_dir/test_hrtimer"
