#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -Ihsa/src -Ihsa/include -Ihsa/third_party/hsa/include \
  hsa/src/ipc_memory.cpp hsa/tests/test_ipc_lifetime.cpp -o "$test_dir/test_ipc_lifetime"
"$test_dir/test_ipc_lifetime"
