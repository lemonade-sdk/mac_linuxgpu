#!/usr/bin/env bash
# linuxu process substrate: task swap, per-process fd tables, mmu notifier
# get/put and exit order, VMA registry, uaccess and SIGKILL wakeups.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers)
common=(linuxu/tests/test_process_substrate.c
  linuxu/src/shims/{process,kthread,fd,dma_fence,timekeeping}.c third_party/linux/lib/rbtree.c
  linuxu/src/mm/{mm,mmu_notifier,uaccess}.c
  linuxu/src/{rcu,rwsem,delay,sync,bug}.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c)
clang "${flags[@]}" "${common[@]}" linuxu/src/shims/task.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_process_substrate"
"$test_dir/test_process_substrate"
# The dext's current: task.c's DriverKit thread-local storage path.
clang "${flags[@]}" -DLINUXU_DEXT_DK -c linuxu/src/shims/task.c -o "$test_dir/task-dk.o"
clang "${flags[@]}" "${common[@]}" "$test_dir/task-dk.o" linuxu/tests/dk_tls_host.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_process_substrate_dk"
"$test_dir/test_process_substrate_dk"
