#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -Dclock_gettime=linuxu_test_clock_gettime \
  linuxu/tests/test_clock_domains.c linuxu/src/shims/timekeeping.c \
  -Wl,-dead_strip -o "$test_dir/test_clock_domains"
"$test_dir/test_clock_domains"
