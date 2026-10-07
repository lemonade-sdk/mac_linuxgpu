#!/usr/bin/env bash
# Client stores into the GPU's BARs kept off a device that is going: a
# submission's stores (kernargs into VRAM, the HDP flush, the doorbell) as
# one gate bracket, under ThreadSanitizer and AddressSanitizer
# (linuxu/tests/test_bar_write_gate.c), and a client's revocation of its
# BAR mapping under writers that use no gate
# (linuxu/tests/test_bar_mapping_retire.c).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for sanitizer in thread address,undefined; do
  clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=$sanitizer -fno-sanitize-recover=all \
    -Idext/sources linuxu/tests/test_bar_write_gate.c -lpthread -o "$test_dir/test_bar_write_gate"
  echo "($sanitizer)"
  ASAN_OPTIONS=detect_leaks=0 "$test_dir/test_bar_write_gate"
done
clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ihsa/src linuxu/tests/test_bar_mapping_retire.c -lpthread -o "$test_dir/test_bar_mapping_retire"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/test_bar_mapping_retire"
