#!/usr/bin/env bash
# The driver's device power state machine (dext/sources/power_state.h) and
# the runtime's copy of its protocol (hsa/abi/amdgpu_power_abi.h).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -Idext/sources linuxu/tests/test_power_state.cpp \
  -o "$test_dir/test_power_state"
"$test_dir/test_power_state"
