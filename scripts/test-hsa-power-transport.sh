#!/usr/bin/env bash
# Device power on the production IOKit transport (hsa/src/power.h): offline
# refusals keep the session healthy; the power snapshot and requests; a
# driver that predates the protocol declines cleanly. IOKit calls replaced.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
sanitize=(-fsanitize=address,undefined -fno-sanitize-recover=all)
# The transport's own sources, as test-hsa-shim-init.sh builds them.
sources=(hsa/src/device_init.cpp hsa/src/isa_target.cpp hsa/src/host_window.cpp)
[ -f hsa/src/allocation_census.cpp ] && sources+=(hsa/src/allocation_census.cpp)
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -ffunction-sections -fdata-sections \
  -Ihsa/src -Ihsa/third_party/hsa/include -Ihost \
  hsa/tests/test_transport_power.cpp "${sources[@]}" \
  -framework IOKit -framework CoreFoundation -Wl,-dead_strip \
  -o "$test_dir/test_transport_power"
"$test_dir/test_transport_power"
