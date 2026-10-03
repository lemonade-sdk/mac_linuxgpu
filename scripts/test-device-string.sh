#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/device-string.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
source = Path('third_party/linux/drivers/gpu/drm/amd/amdgpu/psp_v14_0.c').read_text()
def function(signature):
    assert source.count(signature) == 1
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    for end in range(opening + 1, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1] + '\n'
    raise RuntimeError(signature)
out.joinpath('upstream_psp_staging.inc').write_text(
    function('static int psp_v14_0_bootloader_load_component(') +
    function('static int psp_v14_0_bootloader_load_kdb('))
config = Path('mk/host_clang.mk').read_text()
paths = re.search(r'INCPATHS := \\\n(.*?)(?:\n\n)', config, re.S)
out.joinpath('includes').write_text('\n'.join(re.findall(r'-I[^\s\\]+', paths[1])))
PY
includes=()
while IFS= read -r path || [[ -n "$path" ]]; do includes+=("$path"); done < "$work/includes"
clang -w -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 \
  -include linux/autoconf.h -g -O2 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  "${includes[@]}" -I"$work" linuxu/tests/test_device_string.c \
  linuxu/src/kmem/device_string.c -Wl,-dead_strip -o "$work/test"
set +e
"$work/test" --legacy
legacy_status=$?
set -e
[[ "$legacy_status" == 86 ]] || { echo "legacy clear was not detected"; exit 1; }
"$work/test"

# Use Make's production flags: this fails if the forced header is removed,
# rather than only proving that the standalone backend itself is safe.
cat > "$work/flags.mk" <<'EOF'
.PHONY: device-string-flags
device-string-flags:
	@printf '%s\n' '$(DK_CFLAGS) $(INCPATHS)'
EOF
make --no-print-directory -f Makefile -f "$work/flags.mk" device-string-flags > "$work/flags"
read -r -a production_flags < "$work/flags"
xcrun clang "${production_flags[@]}" -c third_party/linux/drivers/gpu/drm/amd/amdgpu/psp_v14_0.c -o "$work/psp.o"
xcrun nm -u "$work/psp.o" > "$work/psp-undefined.txt"
python3 - "$work/psp-undefined.txt" <<'PY'
from pathlib import Path
import sys
symbols = set(Path(sys.argv[1]).read_text().split())
assert {'_linuxu_device_memset', '_linuxu_device_memcpy'} <= symbols, symbols
assert not {'___memset_chk', '___memcpy_chk', '___memmove_chk'} & symbols, symbols
PY

# Check real arm64 DriverKit code generation, including optimized builds.
for optimization in 0 2; do
  xcrun clang "${production_flags[@]}" -O"$optimization" \
    -c linuxu/src/kmem/device_string.c -o "$work/backend.o"
  xcrun llvm-objdump --macho --disassemble "$work/backend.o" > "$work/backend.s"
  xcrun nm -u "$work/backend.o" > "$work/undefined.txt"
  python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
asm = p.joinpath('backend.s').read_text()
assert not re.search(r'\bdc\s+zva\b|\b[qlv][0-9]+\b|\bv[0-9]+\.', asm), asm
assert not p.joinpath('undefined.txt').read_text().strip(), 'backend calls another library'
PY
done
echo 'Production PSP object: shim memset/memcpy calls, no fortified libc bulk calls'
echo 'DriverKit memory backend: scalar instructions only; no external calls at -O0/-O2'
