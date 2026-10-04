#!/usr/bin/env bash
# The properties the dext publishes on its IOService for System Information
# and our tools (dext/sources/device_properties.h), built offline with
# DriverKit container substitutes (linuxu/tests/test_device_properties.cpp).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -Ilinuxu/headers -Idext/sources \
  linuxu/tests/test_device_properties.cpp -o "$test_dir/test_device_properties"
"$test_dir/test_device_properties"
