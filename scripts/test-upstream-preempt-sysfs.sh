#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
source = Path('third_party/linux/drivers/gpu/drm/ttm/ttm_resource.c').read_text()
parts=[]
for signature in ('void ttm_resource_manager_init(', 'uint64_t ttm_resource_manager_usage('):
    start=source.index(signature); opening=source.index('{',start); depth=1
    for end in range(opening+1,len(source)):
        depth += (source[end]=='{') - (source[end]=='}')
        if not depth:
            parts.append(source[start:end+1]);break
    else: raise SystemExit('missing upstream function body')
out.joinpath('preempt_resource_production.inc').write_text('\n'.join(parts))
config=Path('mk/host_clang.mk').read_text()
paths=re.search(r'INCPATHS := \\\n(.*?)(?:\n\n)',config,re.S)
out.joinpath('includes').write_text('\n'.join(re.findall(r'-I[^\s\\]+',paths[1])))
PY
includes=()
while IFS= read -r path || [[ -n "$path" ]]; do includes+=("$path"); done < "$work/includes"
common=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections)
objects=()
for source in third_party/linux/drivers/gpu/drm/drm_print.c linuxu/src/shims/sysfs.c linuxu/src/sysfs.c linuxu/src/kmem/{slab,kobject,kmemalloc,kmemcheck}.c \
  linuxu/src/shims/{printk,task,kthread}.c linuxu/src/{sync,bug,spinlock,delay}.c linuxu/tests/test_upstream_preempt_sysfs.c; do
  flags=()
  [[ "$source" != linuxu/src/shims/sysfs.c ]] || flags=(-Dcalloc=sysfs_test_calloc)
  object="$work/${source//\//_}.o"
  clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h "${common[@]}" "${flags[@]}" "${includes[@]}" -I"$work" -c "$source" -o "$object"
  objects+=("$object")
done
clang "${common[@]}" "${objects[@]}" -Wl,-dead_strip -lpthread -o "$work/test_preempt_sysfs"
"$work/test_preempt_sysfs"
