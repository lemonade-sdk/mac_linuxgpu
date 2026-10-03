#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DLINUXU_DEXT=1 -Ilinuxu/headers linuxu/tests/test_native_ipc.c \
  dext/sources/dext_compute.c -o "$test_dir/test_native_ipc"
for scenario in lifetime failed-detach failed-final-free exhaustion; do
  "$test_dir/test_native_ipc" "$scenario"
done
