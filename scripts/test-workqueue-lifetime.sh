#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -Ilinuxu/headers \
  linuxu/tests/test_workqueue_lifetime.c -lpthread -o "$test_dir/test_workqueue_lifetime"
"$test_dir/test_workqueue_lifetime"
