#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
mkdir -p "$test_dir/include/DriverKit" "$test_dir/include/PCIDriverKit"
for header in IOService IOBufferMemoryDescriptor IODMACommand IOMemoryMap IOLib; do
  printf '#include "driverkit_dma_mocks.h"\n' > "$test_dir/include/DriverKit/$header.h"
done
printf '#include "driverkit_dma_mocks.h"\n' > "$test_dir/include/PCIDriverKit/IOPCIDevice.h"
# These include paths replace all DriverKit APIs; the binary links no driver framework.
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  -c linuxu/src/kmem/device_string.c -o "$test_dir/device_string.o"
clang++ -x objective-c++ -std=c++17 -fno-exceptions -fno-objc-exceptions -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -DLINUXU_DEXT=1 \
  -I"$test_dir/include" -Ilinuxu/tests -idirafter linuxu/headers \
  linuxu/tests/test_iokit_dma.cpp -x none "$test_dir/printk.o" "$test_dir/device_string.o" \
  -Wl,-dead_strip -o "$test_dir/test_iokit_dma"
"$test_dir/test_iokit_dma"
"$test_dir/test_iokit_dma" failed-completion
"$test_dir/test_iokit_dma" unpublished
"$test_dir/test_iokit_dma" shutdown-reset-failed
"$test_dir/test_iokit_dma" shutdown-complete-failed
"$test_dir/test_iokit_dma" probe-commit-failed
"$test_dir/test_iokit_dma" quarantine-prepare
"$test_dir/test_iokit_dma" quarantine-complete
"$test_dir/test_iokit_dma" orphaned-bar0
"$test_dir/test_iokit_dma" quarantine-release
"$test_dir/test_iokit_dma" import
"$test_dir/test_iokit_dma" large
