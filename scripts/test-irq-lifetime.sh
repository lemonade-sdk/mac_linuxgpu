#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -ffunction-sections -fdata-sections -Ilinuxu/headers)
clang "${flags[@]}" -DLINUXU_DEXT_DK=1 linuxu/tests/test_irq_lifetime.c \
  linuxu/src/amdgpu-rt/device.c linuxu/src/spinlock.c linuxu/src/pci/pci_stub.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_irq_lifetime"
"$test_dir/test_irq_lifetime"
clang "${flags[@]}" linuxu/tests/test_irq_seam.c linuxu/src/pci/pci_irq_seam.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_irq_seam"
"$test_dir/test_irq_seam"
echo 'IRQ disable/drain, ownership and per-thread context checks passed'
