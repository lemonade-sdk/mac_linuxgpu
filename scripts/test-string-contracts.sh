#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/string-contracts.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_string_contracts.c linuxu/src/kmem/string.c \
  -Wl,-dead_strip -o "$work/test_string_contracts"
"$work/test_string_contracts"
