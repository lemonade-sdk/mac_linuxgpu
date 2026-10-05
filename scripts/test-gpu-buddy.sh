#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/gpu-buddy.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/vram-init.inc" "$work/pinned-buddy.c" <<'PY'
import hashlib
from pathlib import Path
import sys

source = Path('third_party/linux/drivers/gpu/buddy.c').read_bytes()
assert hashlib.sha256(source).hexdigest() == 'f60721fc387f687b7e9e37f221e69adae3c2813485fd5c3d1d7c170b55f630d8'
# The pinned file, each declared patch reversed (patches/linux/): the
# negative controls run against it.
fix = b'\tif (gpu_buddy_block_is_free(block) && !RB_EMPTY_NODE(&block->rb))\n\t\trbtree_remove(mm, block);\n\n'
topdown = (b'\t\tif (flags & GPU_BUDDY_TOPDOWN_ALLOCATION) {\n'
           b'\t\t\tblock = get_maxblock(mm, order, tree);\n'
           b'\t\t\tif (block)\n'
           b'\t\t\t\ttmp = gpu_buddy_block_order(block);\n'
           b'\t\t} else {\n'
           b'\t\t\tfor (tmp = order; tmp <= mm->max_order; ++tmp) {\n'
           b'\t\t\t\troot = &mm->free_trees[tree][tmp];\n'
           b'\t\t\t\tblock = rbtree_last_free_block(root);\n'
           b'\t\t\t\tif (block)\n'
           b'\t\t\t\t\tbreak;\n'
           b'\t\t\t}\n'
           b'\t\t}\n')
pinned_loop = (b'\t\tfor (tmp = order; tmp <= mm->max_order; ++tmp) {\n'
               b'\t\t\troot = &mm->free_trees[tree][tmp];\n'
               b'\t\t\tblock = rbtree_last_free_block(root);\n'
               b'\t\t\tif (block)\n'
               b'\t\t\t\tbreak;\n'
               b'\t\t}\n')
assert source.count(fix) == 1 and source.count(topdown) == 1
original = source.replace(fix, b'', 1).replace(topdown, pinned_loop, 1)
assert hashlib.sha256(original).hexdigest() == '61db573ffe054f56fd7e49253d1511ac4be1b616f8fb0395caaf23ed6ee7df95'
Path(sys.argv[2]).write_bytes(original)
driver = Path('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c').read_text()
signature = 'int amdgpu_vram_mgr_init(struct amdgpu_device *adev)\n{'
assert driver.count(signature) == 1
start = driver.index(signature)
end = driver.index('\n}\n', start) + 3
body = driver[start:end]
assert body.count('gpu_buddy_init(') == 1
Path(sys.argv[1]).write_text(body)
PY
common=(-w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h
        -fsanitize=address,undefined -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections -Ilinuxu/headers -I"$work")
heap=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
      -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
      -Dfree=dext_test_free -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
clang "${common[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work/backend.o"
clang "${common[@]}" "${heap[@]}" -DLINUXU_DEXT_DK=1 \
  -c linuxu/src/shims/dext_alloc.c -o "$work/heap.o"
clang "${common[@]}" "${heap[@]}" linuxu/tests/test_gpu_buddy.c \
  third_party/linux/drivers/gpu/buddy.c third_party/linux/lib/rbtree.c \
  linuxu/src/kmem/{kmemalloc,kmemcheck}.c "$work/backend.o" "$work/heap.o" \
  -Wl,-dead_strip -lpthread -o "$work/test_gpu_buddy"
"$work/test_gpu_buddy"
clang "${common[@]}" "${heap[@]}" linuxu/tests/test_gpu_buddy.c \
  "$work/pinned-buddy.c" third_party/linux/lib/rbtree.c \
  linuxu/src/kmem/{kmemalloc,kmemcheck}.c "$work/backend.o" "$work/heap.o" \
  -Wl,-dead_strip -lpthread -o "$work/test_pinned_buddy"
# The pinned allocator drops top-down in the clear tree's fallback: the
# 1 MiB request takes the low block.
"$work/test_pinned_buddy" --topdown-negative-control | grep -q "fallback took the block at 2 MiB" || {
  echo "Pinned allocator negative control did not take the low block" >&2; exit 1; }
echo "Pinned negative control: a clear top-down request fell back to the low block"
python3 - "$work/test_pinned_buddy" <<'PY'
import os
from pathlib import Path
import subprocess
import sys

env = dict(os.environ, ASAN_OPTIONS='abort_on_error=0:halt_on_error=1:detect_leaks=0')
result = subprocess.run([sys.argv[1], '--range-split-negative-control'],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, env=env, timeout=30)
required = ('ERROR: AddressSanitizer: heap-use-after-free', 'kmem_cache_destroy', 'split_block')
if result.returncode == 0 or not all(text in result.stdout for text in required):
    sys.stderr.write(result.stdout)
    raise SystemExit('Pinned allocator negative control did not reproduce the expected rollback UAF')
lines = result.stdout.splitlines()
point = [line for line in lines if line.startswith('range-split failure point:')][-1]
selected = [line for line in lines if any(text in line for text in required) or line.startswith('SUMMARY:')]
diagnostic = Path('build/diagnostics/gpu-buddy-negative-control.log')
diagnostic.parent.mkdir(parents=True, exist_ok=True)
diagnostic.write_text('Pinned drivers/gpu/buddy.c negative control\n'
                      'SHA256: 61db573ffe054f56fd7e49253d1511ac4be1b616f8fb0395caaf23ed6ee7df95\n'
                      'Both declared patches were reversed.\n'
                      + point + '\n' + '\n'.join(selected) + '\n')
print(f'Pinned negative control reproduced the expected ASan rollback UAF; {diagnostic}')
PY
