#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -f build/libmacamgdu.a || {
  echo 'build/libmacamgdu.a missing; run make lib first' >&2
  exit 1
}
mkdir -p build/tests
clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h \
  -Ilinuxu/headers linuxu/tests/test_driver_bootstrap_integration.c \
  build/libmacamgdu.a -lpthread -o build/tests/test_driver_bootstrap_integration
build/tests/test_driver_bootstrap_integration
