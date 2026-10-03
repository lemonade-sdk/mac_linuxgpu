#!/usr/bin/env bash
# The Linux-file RPC core (linuxu/src/amdgpu-rt/lx_files.c, lx_frame.c,
# lx_describe.c) on the linuxu process substrate, against fixture render
# and KFD character devices: per-client descriptor tables, nested ioctl
# memory, async waits, argument pages, PFN mmaps, teardown.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers)
clang "${flags[@]}" linuxu/tests/test_lx_files.c \
  linuxu/src/amdgpu-rt/{lx_files,lx_frame,lx_describe}.c \
  linuxu/src/shims/{process,task,kthread,fd,chrdev,module,dma_fence,timekeeping,printk}.c \
  third_party/linux/lib/rbtree.c \
  linuxu/src/mm/{mm,mmu_notifier,uaccess,page}.c linuxu/src/kmem/slab.c linuxu/src/dart/dart.c \
  linuxu/src/{rcu,rwsem,delay,sync,bug}.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_lx_files"
"$test_dir/test_lx_files"
