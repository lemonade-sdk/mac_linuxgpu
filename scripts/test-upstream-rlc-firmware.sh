#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# The RLC images are linux-firmware files fetched at build time.
export FIRMWARE_DIR="${FIRMWARE_DIR:-build/firmware/amdgpu}"
for image in gc_12_0_0_rlc.bin gc_12_0_1_rlc.bin; do
  if [[ ! -f "$FIRMWARE_DIR/$image" ]]; then
    echo "SKIP test-upstream-rlc-firmware: $FIRMWARE_DIR/$image is missing (run scripts/fetch-firmware.sh)"
    exit 0
  fi
done
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

python3 - "$test_dir" <<'PY'
from pathlib import Path
import re
import sys

out = Path(sys.argv[1])
def function(path, signature):
    text = Path(path).read_text()
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    for end in range(opening + 1, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if not depth:
            return text[start:end+1] + '\n'
    raise SystemExit(f'Unterminated upstream function: {signature}')

out.joinpath('upstream_rlc_init.inc').write_text(function(
    'third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c',
    'static int amdgpu_gfx_rlc_init_microcode_v2_0('))
out.joinpath('upstream_rlc_cleanup.inc').write_text(function(
    'third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c', 'void amdgpu_ucode_release(') + function(
    'third_party/linux/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c', 'static void gfx_v12_0_free_microcode('))
# Read only the existing include-path assignment; no shared build is run.
config = Path('mk/host_clang.mk').read_text()
paths = re.search(r'INCPATHS := \\\n(.*?)(?:\n\n)', config, re.S)
if not paths:
    raise SystemExit('Missing production include paths')
out.joinpath('includes').write_text('\n'.join(re.findall(r'-I[^\s\\]+', paths[1])))
PY

includes=()
while IFS= read -r path || [[ -n "$path" ]]; do includes+=("$path"); done < "$test_dir/includes"
clang -w -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 \
  -include linux/autoconf.h -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  "${includes[@]}" -I"$test_dir" linuxu/tests/test_upstream_rlc_firmware.c \
  linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c \
  linuxu/src/shims/firmware.c linuxu/src/shims/printk.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_upstream_rlc_firmware"
"$test_dir/test_upstream_rlc_firmware" --legacy-zero
"$test_dir/test_upstream_rlc_firmware" "$@"
