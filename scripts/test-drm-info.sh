#!/usr/bin/env bash
# The AMDGPU_INFO reader (linuxu/src/amdgpu-rt/drm_info.c) on the linuxu
# process substrate, against a fixture render node.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers)
clang "${flags[@]}" linuxu/tests/test_drm_info.c \
  linuxu/src/amdgpu-rt/{drm_info,process_file}.c \
  linuxu/src/shims/{process,task,kthread,fd,chrdev,module,dma_fence,timekeeping,printk}.c \
  third_party/linux/lib/rbtree.c \
  linuxu/src/mm/{mm,mmu_notifier,uaccess}.c \
  linuxu/src/{rcu,rwsem,delay,sync,bug}.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_drm_info"
"$test_dir/test_drm_info"
