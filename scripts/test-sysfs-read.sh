#!/usr/bin/env bash
# In-memory sysfs tree and reads: linuxu/src/shims/sysfs.c with the kobject,
# hwmon and PCI attribute code it serves.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_sysfs_read.c linuxu/src/shims/sysfs.c linuxu/src/kmem/kobject.c \
  linuxu/src/shims/hwmon.c linuxu/src/pci/pci_sysfs.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_sysfs_read"
"$test_dir/test_sysfs_read"
