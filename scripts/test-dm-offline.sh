#!/usr/bin/env bash
# Offline Display Core probe and display test: the unmodified amdgpu_dm IP
# block and Display Core against a fixture DCN 4.0.1 device, the registered
# DRM device's connector sysfs, and the in-driver display test client
# (linuxu/tests/test_dm_offline.c). The fixture runs no DMCUB firmware, so
# DMUB and power-gate waits time out; a run takes a few minutes.
# Links the host library (make lib).
set -euo pipefail
cd "$(dirname "$0")/.."
make -s lib >/dev/null
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/flags.mk" <<'MAKE'
.PHONY: print-includes
print-includes:
	@printf '%s\n' $(INCPATHS)
MAKE
include_string=$(make -s -f Makefile -f "$work/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
clang -w -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 -include linux/autoconf.h \
  -g -O1 -fsanitize=address "${includes[@]}" linuxu/tests/test_dm_offline.c linuxu/tests/dcn401_fixture.c build/libmacamgdu.a \
  -lpthread -o "$work/test_dm_offline"
"$work/test_dm_offline"
