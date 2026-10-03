#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/sysinfo-ttm.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/upstream-memory.inc" <<'PY'
from pathlib import Path
import sys

def function(path, signature):
    source = Path(path).read_text()
    assert source.count(signature) == 1
    begin = source.index(signature)
    end = source.index('\n}\n', begin) + 3
    return source[begin:end]

functions = [
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c', 'static inline u64 ttm_get_node_memory_size(int nid)'),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_tt.c', 'void ttm_tt_mgr_init(unsigned long num_pages, unsigned long num_dma32_pages)'),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_tt.c', 'unsigned long ttm_tt_pages_limit(void)'),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'static int ttm_global_init(void)'),
    ('third_party/linux/drivers/gpu/drm/ttm/ttm_device.c', 'static void ttm_global_release(void)'),
]
Path(sys.argv[1]).write_text('\n'.join(function(*args) for args in functions))
PY
common=(-w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h
        -fsanitize=address,undefined -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections -Ilinuxu/headers -I"$work")
for platform in host driverkit; do
  flags=(-Dsysctlbyname=test_sysctlbyname)
  if [[ "$platform" == driverkit ]]; then flags=(-DLINUXU_DEXT_DK=1); fi
  clang "${common[@]}" "${flags[@]}" linuxu/tests/test_sysinfo_ttm.c \
    linuxu/src/shims/sysinfo.c -Wl,-dead_strip -lpthread -o "$work/test-$platform"
  for scenario in denied truncated zero bad-page free-overflow retry native4k valid free-failure; do
    "$work/test-$platform" "$scenario"
  done
done
