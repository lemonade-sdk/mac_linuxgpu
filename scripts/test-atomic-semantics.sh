#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -Ilinuxu/headers \
  linuxu/tests/test_atomic_semantics.c linuxu/src/refcount.c \
  -lpthread -o "$test_dir/test_atomic_semantics"
"$test_dir/test_atomic_semantics"
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -DLINUXU_DEXT_DK=1 -Ilinuxu/headers \
  linuxu/tests/test_atomic_semantics.c linuxu/src/refcount.c \
  -lpthread -o "$test_dir/test_atomic_semantics_dk"
"$test_dir/test_atomic_semantics_dk"
