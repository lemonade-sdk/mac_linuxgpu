#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/shmem-ownership.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers -Ithird_party/linux/include \
  linuxu/tests/test_shmem_ownership.c third_party/linux/drivers/gpu/drm/ttm/ttm_backup.c \
  linuxu/src/mm/{page,shmem}.c linuxu/src/shims/fd.c linuxu/src/dart/dart.c \
  linuxu/src/{xarray,rcu,sync,bug}.c linuxu/src/shims/{task,kthread}.c linuxu/src/kmem/{kmemalloc,kmemcheck}.c \
  -Wl,-dead_strip -lpthread -o "$work/test_shmem_ownership"
"$work/test_shmem_ownership"
