#!/usr/bin/env bash
# Fuzz the Linux-file RPC framing (linuxu/src/amdgpu-rt/lx_frame.c): the
# dext's request checker against a reference, encoder rules, and the
# client's reply decoder. Plain userspace C, as the client library builds it.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -idirafter linuxu/headers \
  linuxu/tests/test_lx_frame_fuzz.c linuxu/src/amdgpu-rt/lx_frame.c \
  -o "$test_dir/test_lx_frame_fuzz"
"$test_dir/test_lx_frame_fuzz"
