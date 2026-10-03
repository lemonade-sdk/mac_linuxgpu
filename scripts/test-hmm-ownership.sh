#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/hmm-ownership.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_hmm_ownership.c linuxu/src/mm/{page,hmm,mm,mmu_notifier}.c \
  linuxu/src/dart/dart.c linuxu/src/{rcu,rwsem,delay,bug}.c \
  linuxu/src/shims/{fd,task}.c third_party/linux/lib/rbtree.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  -Wl,-dead_strip -lpthread -o "$work/test_hmm_ownership"
"$work/test_hmm_ownership"
