#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_task_kthread.c linuxu/src/shims/task.c \
  linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_task_kthread"
"$test_dir/test_task_kthread"
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -DLINUXU_DEXT_DK \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_task_tls_failure.c linuxu/src/shims/task.c \
  linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_task_tls_failure"
for failure in create publish restore; do
  "$test_dir/test_task_tls_failure" "$failure"
done
