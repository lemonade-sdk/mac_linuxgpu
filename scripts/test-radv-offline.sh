#!/usr/bin/env bash
# RADV offline: Mesa's Vulkan driver (build/radv, scripts/build-radv.sh) on
# the software GPU of the CS fixture device, through libdrm-mlg and
# libmlg_drm's loopback transport into upstream DRM/amdgpu. Device
# creation, memory, submits with fences and semaphores, CP DMA and
# WRITE_DATA results, an ACO-compiled compute pipeline and a dispatch
# (vulkan/tests/test_radv_offline.c).
#
# Skips (exit 0) when RADV has not been built; RADV_REQUIRED=1 makes that a
# failure.
set -euo pipefail
cd "$(dirname "$0")/.."
icd=build/radv/install/lib/libvulkan_radeon.dylib
if [ ! -f "$icd" ]; then
  if [ -n "${RADV_REQUIRED:-}" ]; then
    echo "test-radv-offline: $icd missing (scripts/build-radv.sh)" >&2
    exit 1
  fi
  echo "test-radv-offline: SKIP (RADV not built; run scripts/build-radv.sh)"
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
              linuxu/tests/cs_fixture.c linuxu/tests/lx_loopback.c; do
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
# The client: Vulkan headers from the pinned Mesa, the package's libmlg_drm
# shared (the image libdrm-mlg loads into the ICD, so the loopback
# transport set here is the one RADV's requests take).
clang -std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1 \
  -Ithird_party/mesa/include -Ilibmlg_drm/include -Ilinuxu/tests -Ivulkan/tests \
  -c vulkan/tests/test_radv_offline.c -o "$work/test_radv_offline.o"
objects+=("$work/test_radv_offline.o")
lib=$(cd $pkg && pwd)
clang "${objects[@]}" build/libmacamgdu.a -L"$lib" -lmlg_drm -Wl,-rpath,"$lib" \
  -framework IOKit -framework CoreFoundation -lpthread -o "$work/test_radv_offline"
# RADV_TEST_KEEP=<path> keeps the binary for a debugger; CS_FIXTURE_TRACE=1
# traces what the software engines execute. No shader cache, so a cached
# pipeline cannot hide the compiler.
if [ -n "${RADV_TEST_KEEP:-}" ]; then cp "$work/test_radv_offline" "$RADV_TEST_KEEP"; fi
MESA_SHADER_CACHE_DISABLE=true "$work/test_radv_offline" "$(cd "$(dirname "$icd")" && pwd)/$(basename "$icd")"
