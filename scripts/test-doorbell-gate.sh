#!/usr/bin/env bash
# Doorbells clients ring themselves (dext/sources/doorbell_gate.h): client
# threads ringing through their gates while the session queue closes them
# for power transitions, a client's close and the session's close, under
# ThreadSanitizer and under AddressSanitizer (linuxu/tests/test_doorbell_gate.c).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for sanitizer in thread address,undefined; do
  clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=$sanitizer -fno-sanitize-recover=all \
    -Idext/sources linuxu/tests/test_doorbell_gate.c -lpthread -o "$test_dir/test_doorbell_gate"
  echo "($sanitizer)"
  ASAN_OPTIONS=detect_leaks=0 "$test_dir/test_doorbell_gate"
done
