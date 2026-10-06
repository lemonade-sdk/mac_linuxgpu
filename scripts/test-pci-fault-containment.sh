#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/pci-fault.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/DriverKit" "$work/include/PCIDriverKit"
for header in IOService IOBufferMemoryDescriptor IODMACommand IOMemoryMap IOLib; do
  printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/DriverKit/$header.h"
done
printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/PCIDriverKit/IOPCIDevice.h"
python3 - "$work/pci_fault_production.inc" <<'PY'
from pathlib import Path
import sys
s = Path('dext/sources/dext_main.mm').read_text()
def section(start, end):
    assert s.count(start) == 1 and s.count(end) == 1
    a = s.index(start)
    return s[a:s.index(end, a)]
def function(signature):
    assert s.count(signature) == 1
    a = s.index(signature)
    start = s.index('{', a)
    depth = 1
    for end in range(start + 1, len(s)):
        depth += (s[end] == '{') - (s[end] == '}')
        if not depth: return s[a:end + 1] + '\n'
    raise RuntimeError(signature)
text = section('static IOPCIDevice *g_pci;', '/* Primary fake-MMIO token')
text += section('static int          g_transport_fault;', '/* ---- IRQ state')
for signature in ['static bool dext_config_offset_ok(',
                  'extern "C" int dext_pci_config_read32(',
                  'extern "C" int dext_pci_config_write32(',
                  'extern "C" int dext_pci_quarantine(',
                  'static bool dext_pci_quarantine_quiescent_locked(',
                  'extern "C" int dext_pci_quarantine_releasable(',
                  'extern "C" int dext_pci_release_quarantine(']:
    text += function(signature)
Path(sys.argv[1]).write_text(text)
PY
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -c linuxu/src/shims/printk.c -o "$work/printk.o"
# The VRAM aperture the BAR0 mapping publishes (rt/device_string.h).
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -c linuxu/src/kmem/device_string.c -o "$work/device_string.o"
clang++ -x objective-c++ -std=c++17 -fno-exceptions -fno-objc-exceptions \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -DLINUXU_DEXT=1 \
  -I"$work/include" -I"$work" -Ilinuxu/tests -Idext/sources -idirafter linuxu/headers \
  linuxu/tests/test_pci_fault_containment.cpp -x none "$work/printk.o" "$work/device_string.o" \
  -Wl,-dead_strip -o "$work/test"
"$work/test"
"$work/test" prepare
"$work/test" quarantine
"$work/test" fatal
"$work/test" release
