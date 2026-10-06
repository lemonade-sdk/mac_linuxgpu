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
# Device memory reaches these functions only through the VRAM aperture
# (linuxu_aperture_*): its accesses stay in general registers, one aligned
# access at a time. Normal memory is copied with NEON (LD1/ST1, byte
# elements), and nothing uses the cache-zeroing instruction (DC ZVA).
# The DriverKit target's default stack protector guards the bounce buffers:
# its references are the only external ones, ___stack_chk_fail bound to the
# dext's own close-first handler (dext/sources/fatal_close.h) and
# ___stack_chk_guard, the canary DriverKit's libSystem exports.
driverkit_sdk=$(xcrun --sdk driverkit --show-sdk-path)
for optimization in 0 2; do
  xcrun clang "${production_flags[@]}" -O"$optimization" \
    -c linuxu/src/kmem/device_string.c -o "$work/backend.o"
  xcrun llvm-objdump --macho --disassemble "$work/backend.o" > "$work/backend.s"
  xcrun nm -u "$work/backend.o" > "$work/undefined.txt"
  python3 - "$work" "$driverkit_sdk" <<'PY'
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
sdk = Path(sys.argv[2])
asm = p.joinpath('backend.s').read_text()
assert not re.search(r'\bdc\s+zva\b', asm), 'cache-zeroing instruction in the memory backend'
functions, name = {}, None
for line in asm.splitlines():
    label = re.match(r'^(_\w+):$', line)
    if label:
        name = label[1]
        functions[name] = []
    elif name and re.match(r'^\s+[0-9a-f]+:\t', line):
        functions[name].append(line.split('\t', 2)[-1].strip())
aperture = ['_linuxu_aperture_read', '_linuxu_aperture_write', '_linuxu_aperture_copy_in',
            '_linuxu_aperture_copy_out', '_linuxu_aperture_fill']
assert set(aperture) <= set(functions), sorted(functions)
simd = re.compile(r'\b[bhsdq][0-9]+\b|\bv[0-9]+(\.|\b)')
memory = re.compile(r'^(ld|st)\S*\s.*\[(\w+)')
for function in aperture + ['_aperture_access']:
    for instruction in functions.get(function, []):
        access = memory.match(instruction)
        if access and simd.search(instruction):
            assert access[2] in ('sp', 'x29'), f'{function}: vector access off the stack: {instruction}'
undefined = set(p.joinpath('undefined.txt').read_text().split())
assert undefined <= {'___stack_chk_fail', '___stack_chk_guard'}, f'backend calls another library: {undefined}'
handler = Path('dext/sources/fatal_close.h').read_text()
assert re.search(r'__attribute__\(\(no_stack_protector\)\) void __stack_chk_fail\(void\)\n\{', handler), \
    'the dext no longer defines __stack_chk_fail'
assert '___stack_chk_guard' in sdk.joinpath('System/DriverKit/usr/lib/libSystem.tbd').read_text(), \
    'DriverKit libSystem does not export ___stack_chk_guard'
PY
done
echo 'Production PSP object: shim memset/memcpy calls, no fortified libc bulk calls'
echo 'DriverKit memory backend: aperture accesses in general registers, no DC ZVA; only the stack protector is external, at -O0/-O2'
