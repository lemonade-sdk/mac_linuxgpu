#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DLINUXU_DEXT_DK=1 \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_page_alloc_dk.c linuxu/src/dart/dart.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_page_alloc_dk"
"$test_dir/test_page_alloc_dk"
"$test_dir/test_page_alloc_dk" --stream-completion-failure
"$test_dir/test_page_alloc_dk" --unpublished-coherent
"$test_dir/test_page_alloc_dk" --unpublished-stream
"$test_dir/test_page_alloc_dk" --stream-mask-completion-failure
