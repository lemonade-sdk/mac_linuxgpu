#!/usr/bin/env bash
# Doorbells on the delivery thread (dext/sources/kick_table.h): delivery
# threads ringing while the session queue publishes, retires and frees
# queues, closes clients and gates power transitions, under
# ThreadSanitizer and under AddressSanitizer (linuxu/tests/test_kick_table.c).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for sanitizer in thread address,undefined; do
  clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=$sanitizer -fno-sanitize-recover=all \
    -Idext/sources linuxu/tests/test_kick_table.c -lpthread -o "$test_dir/test_kick_table"
  echo "($sanitizer)"
  "$test_dir/test_kick_table"
done
