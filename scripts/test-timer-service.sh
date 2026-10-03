#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_timer_service.c -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_timer_service"
"$test_dir/test_timer_service"
