#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++17 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all linuxu/tests/test_aql_storage_bounds.cpp \
  -o "$test_dir/test_aql_storage_bounds"
"$test_dir/test_aql_storage_bounds"
