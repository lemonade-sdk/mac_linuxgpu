#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir" <<'PY'
import pathlib, sys
source = pathlib.Path("linuxu/src/amdgpu-rt/device.c").read_text()
def bounded(start, end, lines):
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit("runtime free extraction markers changed")
    begin = source.index(start)
    text = source[begin:source.index(end, begin)]
    if len(text.splitlines()) > lines:
        raise SystemExit("runtime free extraction exceeded bounded section")
    return text
layout = bounded("struct rt_device {", "#ifdef LINUXU_DEXT_DK\nstatic struct rt_device *rt_active_device;", 10)
release = bounded("void rt_device_free(struct rt_device *dev)", "void *rt_ioremap_active", 40)
pathlib.Path(sys.argv[1], "pci_runtime_free.inc").write_text(
    layout + "static struct rt_device *rt_active_device;\n" + release)
PY
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DLINUXU_DEXT_DK=1 -DLINUXU_TEST_RT_DEVICE_FREE=1 \
  -ffunction-sections -fdata-sections -I"$test_dir" -Ilinuxu/headers \
  linuxu/tests/test_pci_lifecycle.c linuxu/src/pci/pci_stub.c \
  linuxu/src/kmem/slab.c linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c \
  "$test_dir/printk.o" -Wl,-dead_strip -lpthread -o "$test_dir/test_pci_lifecycle"
"$test_dir/test_pci_lifecycle"
echo "PASS PCI lifecycle: cleanup ordering, failed-probe owner retention, retry/remove/unregister/runtime-free rejection"
