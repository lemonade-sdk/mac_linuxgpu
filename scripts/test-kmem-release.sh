#!/usr/bin/env bash
# The kmalloc family built as release (no DEBUG: no kmemcheck) and as debug
# (DEBUG=1: kmemcheck), from the production sources (linuxu/src/kmem).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h
  -fsanitize=address,undefined -fno-sanitize-recover=all -Ilinuxu/headers)
for mode in release debug; do
  mode_flags=()
  [ "$mode" = debug ] && mode_flags=(-DDEBUG=1)
  clang "${flags[@]}" "${mode_flags[@]}" linuxu/tests/test_kmem_release.c \
    linuxu/src/kmem/{kmemalloc,kmemcheck}.c linuxu/src/shims/printk.c linuxu/src/spinlock.c \
    -Wl,-dead_strip \
    -o "$test_dir/test_kmem_$mode"
  "$test_dir/test_kmem_$mode"
done
