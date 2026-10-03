#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_devres_concurrency.c linuxu/src/kmem/slab.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_devres_concurrency"
"$test_dir/test_devres_concurrency"
clang -w -std=gnu11 -D__KERNEL__ -g -O1 -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_platform_devres.c linuxu/src/kmem/slab.c \
  linuxu/src/shims/platform_device.c -Wl,-dead_strip -o "$test_dir/test_platform_devres"
"$test_dir/test_platform_devres"
