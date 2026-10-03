#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cat > "$test_dir/flags.mk" <<'MAKE'
.PHONY: print-includes
print-includes:
	@printf '%s\n' $(INCPATHS)
MAKE
include_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
# The real KFD XNACK selection (kfd_process.c); unreferenced upstream code is
# stripped.
clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections "${includes[@]}" \
  linuxu/tests/test_device_topology.c linuxu/src/amdgpu-rt/topology.c \
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process.c linuxu/src/shims/printk.c linuxu/src/bug.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_device_topology"
"$test_dir/test_device_topology"
