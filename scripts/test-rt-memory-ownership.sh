#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include)
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_rt_dma_ownership.c \
  linuxu/src/dart/dart.c linuxu/src/drm/ttm.c linuxu/tests/iosysctl_host.c -Wl,-dead_strip \
  -o "$test_dir/test_rt_dma_ownership"
"$test_dir/test_rt_dma_ownership"
clang "${flags[@]}" -Dcalloc=rt_test_calloc -Dfree=rt_test_free \
  -c linuxu/src/drm/ttm.c -o "$test_dir/ttm.o"
clang "${flags[@]}" linuxu/tests/test_rt_host_ownership.c \
  linuxu/src/amdgpu-rt/device.c "$test_dir/ttm.o" -Wl,-dead_strip \
  -o "$test_dir/test_rt_host_ownership"
"$test_dir/test_rt_host_ownership"
