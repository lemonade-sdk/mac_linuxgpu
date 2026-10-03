#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

# Keep the fixture tied to the exact table and flags compiled into AMDGPU.
python3 - "$test_dir/upstream_pci_table.inc" <<'PY'
from pathlib import Path
import re
import sys

def declaration(path, pattern):
    match = re.search(pattern, Path(path).read_text(), re.S)
    if not match:
        raise SystemExit(f"Missing upstream declaration in {path}")
    return match.group(0)

flags = declaration('third_party/linux/drivers/gpu/drm/amd/include/amd_shared.h', r'enum amd_chip_flags\s*\{.*?\n\};')
table = declaration('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c',
                    r'static const struct pci_device_id pciidlist\[\]\s*=\s*\{.*?\n\};')
Path(sys.argv[1]).write_text(flags + '\n' + table + '\n')
PY

flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections
  -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include -I"$test_dir")
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 -c linuxu/src/pci/pci_stub.c \
  -o "$test_dir/pci_stub.o"
for order in 0 1 2; do
  clang "${flags[@]}" -DTEST_INCLUDE_ORDER="$order" \
    linuxu/tests/test_upstream_pci_matching.c "$test_dir/pci_stub.o" \
    -Wl,-dead_strip -o "$test_dir/test_matching_$order"
  "$test_dir/test_matching_$order"
done
