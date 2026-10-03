#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/shmem-driverkit.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/DriverKit" "$work/include/PCIDriverKit"
for header in IOService IOBufferMemoryDescriptor IODMACommand IOMemoryMap IOLib; do
  printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/DriverKit/$header.h"
done
printf '#include "driverkit_dma_mocks.h"\n' > "$work/include/PCIDriverKit/IOPCIDevice.h"
common=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
        -ffunction-sections -fdata-sections)
objects=()
for source in linuxu/src/mm/{page,shmem}.c linuxu/src/kmem/{vmalloc,kmemalloc,kmemcheck}.c \
              linuxu/src/shims/{fd,task,kthread,printk}.c linuxu/src/dart/dart.c \
              linuxu/src/{xarray,rcu,sync,bug}.c linuxu/tests/test_shmem_driverkit_alias.c; do
  flags=()
  case "$source" in
    */mm/page.c|*/kmem/vmalloc.c|*/dart/dart.c) flags=(-DLINUXU_DEXT_DK=1) ;;
  esac
  object="$work/$(basename "$source" .c).o"
  clang -w -std=gnu11 "${common[@]}" "${flags[@]}" -Ilinuxu/headers -c "$source" -o "$object"
  objects+=("$object")
done
clang++ -x objective-c++ -std=c++17 -fno-exceptions -fno-objc-exceptions "${common[@]}" \
  -DLINUXU_DEXT=1 -I"$work/include" -Ilinuxu/tests -idirafter linuxu/headers \
  -c linuxu/tests/shmem_driverkit_backend.cpp -o "$work/backend.o"
clang++ "${common[@]}" "${objects[@]}" "$work/backend.o" \
  -Wl,-dead_strip -lpthread -o "$work/test_shmem_driverkit_alias"
"$work/test_shmem_driverkit_alias"
