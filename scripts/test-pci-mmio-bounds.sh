#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir/upstream_bus_status.inc" <<'PY'
from pathlib import Path
import sys
source = Path('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu.h').read_text()
signature = 'static inline int amdgpu_device_bus_status_check('
if source.count(signature) != 1:
    raise SystemExit('upstream bus-status signature changed')
start = source.index(signature)
opening = source.index('{', start)
depth = 1
for end in range(opening + 1, len(source)):
    depth += (source[end] == '{') - (source[end] == '}')
    if not depth:
        Path(sys.argv[1]).write_text(source[start:end + 1] + '\n')
        break
else:
    raise SystemExit('upstream bus-status body missing')
PY
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include -I"$test_dir")
clang "${flags[@]}" -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 -DLINUXU_TEST_UPSTREAM_BUS_STATUS=1 linuxu/tests/test_pci_dext.c \
  linuxu/src/pci/pci_stub.c linuxu/src/dart/dma_mask.c "$test_dir/printk.o" \
  -Wl,-dead_strip -o "$test_dir/test_pci_dext"
"$test_dir/test_pci_dext"
clang "${flags[@]}" -Dcalloc=pci_test_calloc -c linuxu/src/pci/pdev_mmio.c \
  -o "$test_dir/pdev_mmio.o"
clang "${flags[@]}" linuxu/tests/test_pci_mmio_bounds.c "$test_dir/pdev_mmio.o" \
  -Wl,-dead_strip -o "$test_dir/test_pci_mmio_bounds"
"$test_dir/test_pci_mmio_bounds"
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_pci_mmio_dk_bounds.c \
  linuxu/src/pci/pdev_mmio.c -Wl,-dead_strip -o "$test_dir/test_pci_mmio_dk_bounds"
"$test_dir/test_pci_mmio_dk_bounds"
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_mmio_dk_accessors.c \
  linuxu/src/pci/pdev_mmio.c -Wl,-dead_strip -o "$test_dir/test_mmio_dk_accessors"
"$test_dir/test_mmio_dk_accessors"
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_memremap_contract.c \
  -Wl,-dead_strip -o "$test_dir/test_memremap_contract"
"$test_dir/test_memremap_contract"
clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  linuxu/tests/test_pci_reset_policy.cpp -o "$test_dir/test_pci_reset_policy"
"$test_dir/test_pci_reset_policy"
