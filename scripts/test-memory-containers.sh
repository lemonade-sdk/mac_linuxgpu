#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/memory-containers.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang -w -std=gnu11 -g -O1 -DDEBUG=1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_memory_containers.c linuxu/src/xarray.c linuxu/src/rcu.c linuxu/src/sync.c linuxu/src/llist.c \
  linuxu/src/shims/interval_tree.c third_party/linux/lib/rbtree.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  -Wl,-dead_strip -lpthread -o "$work/test_memory_containers"
"$work/test_memory_containers"
