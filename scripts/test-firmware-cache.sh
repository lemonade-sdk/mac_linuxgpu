#!/usr/bin/env bash
# request_firmware cache, embedded fallback generator and the on-demand
# host firmware servicer. Uses synthetic firmware names only, and runs each
# test against an empty embedded table and a generated one.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/fw-cache.XXXXXX")
trap 'rm -rf "$work"' EXIT

# A miniature amdgpu firmware directory: images, a subdirectory, and the
# licensing files the generator must skip.
fwdir="$work/amdgpu"
mkdir -p "$fwdir/sub"
python3 - "$fwdir" <<'PY'
import os, sys
d = sys.argv[1]
def put(name, data):
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)
put("alpha_1_2_3.bin", bytes([0x10]) + bytes(range(1, 80)))
put("sub/beta_4_5_6.bin", bytes([0x20]) * 40)
put("tiny_7_8_9.bin", b"short")           # below the 16-byte minimum
put("WHENCE", b"provenance\n")
put("LICENSE.amdgpu", b"license\n")
PY

python3 scripts/fw2rodata.py --output "$work/embedded.c" --firmware-dir "$fwdir"
python3 scripts/fw2rodata.py --output "$work/empty.c" --empty
grep -q '"amdgpu/alpha_1_2_3.bin"' "$work/embedded.c"
grep -q '"amdgpu/sub/beta_4_5_6.bin"' "$work/embedded.c"
! grep -q 'WHENCE\|LICENSE\|tiny_7_8_9' "$work/embedded.c"
grep -q 'if (count) \*count = 2;' "$work/embedded.c"
grep -q 'if (count) \*count = 0;' "$work/empty.c"

flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections
  -D__KERNEL__ -include linux/autoconf.h
  -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include -Ilinuxu/src -Ihost)
objects=()
for src in linuxu/src/shims/firmware.c linuxu/src/fw/fw_table.c \
    linuxu/src/fw/fw_mailbox.c linuxu/src/shims/printk.c \
    linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c host/fw_mailbox_service.c; do
  obj="$work/$(basename "${src%.c}").o"
  clang "${flags[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done
for table in empty embedded; do
  clang "${flags[@]}" -c "$work/$table.c" -o "$work/$table.o"
  for test in test_firmware test_firmware_mailbox; do
    clang "${flags[@]}" "linuxu/tests/$test.c" "${objects[@]}" "$work/$table.o" \
      -Wl,-dead_strip -lpthread -o "$work/$test-$table"
    TMPDIR="$work" "$work/$test-$table" > "$work/$test-$table.log" 2>&1 || {
      tail -20 "$work/$test-$table.log"; exit 1; }
    echo "$test ($table embedded table): passed"
  done
done
