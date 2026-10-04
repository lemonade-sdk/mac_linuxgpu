#!/usr/bin/env bash
# RADV's Metal surfaces on the GPU's display engine, offline: Mesa's Vulkan
# driver (build/radv, scripts/build-radv.sh) on the software GPU of the CS
# fixture, presenting through libdrm-mlg's drmMlgScanout to the display
# output of the fixture's DCN 4.0.1 display (vulkan/tests/
# test_radv_scanout.c, linuxu/tests/scanout_fixture.c).
#
# Skips (exit 0) when RADV has not been built; RADV_REQUIRED=1 makes that a
# failure.
set -euo pipefail
cd "$(dirname "$0")/.."
icd=build/radv/install/lib/libvulkan_radeon.dylib
if [ ! -f "$icd" ]; then
  if [ -n "${RADV_REQUIRED:-}" ]; then
    echo "test-radv-scanout: $icd missing (scripts/build-radv.sh)" >&2
    exit 1
  fi
  echo "test-radv-scanout: SKIP (RADV not built; run scripts/build-radv.sh)"
  exit 0
fi
make -s lib libdrm-mlg >/dev/null
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat > "$work/flags.mk" <<'MAKE'
.PHONY: print-includes
print-includes:
	@printf '%s\n' $(INCPATHS)
.PHONY: print-hostcflags
print-hostcflags:
	@printf '%s\n' $(filter-out -MMD -MP -DLINUXU_RT_HOST_SHADOW=1,$(HOSTCFLAGS))
MAKE
include_string=$(make -s -f Makefile -f "$work/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
host_string=$(make -s -f Makefile -f "$work/flags.mk" print-hostcflags | tr '\n' ' ')
read -r -a hostcflags <<< "$host_string"
kernel=("${hostcflags[@]}" -g -O1 "${includes[@]}")
objects=()
# Plain-memory GART and VRAM accessors for the fixture (test-cs-selftest.sh).
for source in third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c \
              third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c \
              linuxu/tests/cs_fixture.c linuxu/tests/dcn401_fixture.c linuxu/tests/lx_loopback.c \
              linuxu/tests/scanout_fixture.c; do
  object="$work/k_$(basename "$source" .c).o"
  clang "${kernel[@]}" -Ilibmlg_drm/include -c "$source" -o "$object"
  objects+=("$object")
done
# The package carries its own libdrm-mlg: bring it up to date with the
# sources, as scripts/build-radv.sh installs it.
pkg=build/radv/install/lib
for dylib in libdrm_mlg.dylib libmlg_drm.dylib; do
  if ! cmp -s build/libdrm-mlg/lib/$dylib $pkg/$dylib; then
    install -m 755 build/libdrm-mlg/lib/$dylib $pkg/
    codesign --force --sign - $pkg/$dylib 2>/dev/null
  fi
done
clang -std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1 \
  -Ithird_party/mesa/include -Ilibmlg_drm/include -Ilinuxu/tests \
  -c vulkan/tests/test_radv_scanout.c -o "$work/test_radv_scanout.o"
clang -Wall -Werror -fno-objc-arc -g -O1 -c vulkan/tests/metal_layer.m -o "$work/metal_layer.o"
objects+=("$work/test_radv_scanout.o" "$work/metal_layer.o")
lib=$(cd $pkg && pwd)
clang "${objects[@]}" build/libmacamgdu.a -L"$lib" -lmlg_drm -Wl,-rpath,"$lib" \
  -framework IOKit -framework CoreFoundation -framework QuartzCore -framework Foundation -framework AppKit \
  -lpthread -o "$work/test_radv_scanout"
if [ -n "${RADV_TEST_KEEP:-}" ]; then cp "$work/test_radv_scanout" "$RADV_TEST_KEEP"; fi
MESA_SHADER_CACHE_DISABLE=true "$work/test_radv_scanout" "$(cd "$(dirname "$icd")" && pwd)/$(basename "$icd")"
