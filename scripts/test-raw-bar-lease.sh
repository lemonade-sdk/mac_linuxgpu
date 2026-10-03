#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all linuxu/tests/test_raw_bar_lease.cpp -o "$test_dir/test_raw_bar_lease"
"$test_dir/test_raw_bar_lease"
