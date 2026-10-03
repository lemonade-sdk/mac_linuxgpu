#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work/kfd_interrupt_helpers.inc" <<'PY'
from pathlib import Path
import sys
source = Path('third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_interrupt.c').read_text()
functions = []
for signature in ('bool enqueue_ih_ring_entry(', 'static bool dequeue_ih_ring_entry('):
    assert source.count(signature) == 1
    begin = source.index(signature)
    end = source.index('\n}\n', begin) + 3
    functions.append(source[begin:end])
Path(sys.argv[1]).write_text('\n'.join(functions))
PY
clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h \
  -include linux/kernel.h -include linux/bug.h \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers -I"$work" \
  linuxu/tests/test_kernel_helpers.c third_party/linux/lib/kfifo.c \
  third_party/linux/lib/list_sort.c third_party/linux/lib/crc/crc16.c \
  linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c linuxu/src/bug.c \
  -Wl,-dead_strip -lpthread -o "$work/test_kernel_helpers"
"$work/test_kernel_helpers"
