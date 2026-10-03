#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/ttm-lru-cleanup.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/upstream-ttm-lru.inc" <<'PY'
from pathlib import Path
import sys
source = Path('third_party/linux/drivers/gpu/drm/ttm/ttm_bo_util.c').read_text()
names = ['static bool ttm_lru_walk_trylock(', 'static int ttm_lru_walk_ticketlock(',
         's64 ttm_lru_walk_for_evict(', 'static void ttm_bo_lru_cursor_cleanup_bo(',
         'void ttm_bo_lru_cursor_fini(', 'struct ttm_bo_lru_cursor *\nttm_bo_lru_cursor_init(',
         'static struct ttm_buffer_object *\n__ttm_bo_lru_cursor_next(',
         'struct ttm_buffer_object *ttm_bo_lru_cursor_next(',
         'struct ttm_buffer_object *ttm_bo_lru_cursor_first(']
functions=[]
for signature in names:
    assert source.count(signature) == 1
    begin=source.index(signature);end=source.index('\n}\n',begin)+3
    functions.append(source[begin:end])
Path(sys.argv[1]).write_text('\n'.join(functions))
PY
clang -w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h \
  -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/ttm -I"$work" \
  linuxu/tests/test_ttm_lru_cleanup.c third_party/linux/drivers/gpu/drm/ttm/ttm_resource.c \
  linuxu/src/drm/dma_resv.c linuxu/src/shims/dma_fence.c \
  linuxu/src/kmem/{kmemalloc,kmemcheck}.c linuxu/src/{sync,rcu,refcount,bug}.c \
  linuxu/src/sync/ww_mutex.c linuxu/src/shims/task.c \
  -Wl,-dead_strip -lpthread -o "$work/test_ttm_lru_cleanup"
"$work/test_ttm_lru_cleanup"
