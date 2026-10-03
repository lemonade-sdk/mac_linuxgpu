#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_page_boundaries.c linuxu/src/mm/page.c linuxu/src/dart/dart.c \
  -Wl,-dead_strip -lpthread -o build/tests/test_page_boundaries
build/tests/test_page_boundaries
