#!/usr/bin/env bash
# Client framebuffers on the display output, offline: a libdrm-mlg client
# (libmlg_drm/libdrm/tests/test_scanout.c) opens the primary node, reads the
# KMS objects, makes framebuffers and shows them with LX_SCANOUT on the
# output the driver runs on the CS fixture's DCN 4.0.1 display
# (linuxu/tests/test_scanout_main.c), through the loopback transport into
# the unmodified upstream DRM/amdgpu and amdgpu_dm. Links the host library
# (make lib) and the shared libdrm-mlg (make libdrm-mlg).
set -euo pipefail
cd "$(dirname "$0")/.."
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
              linuxu/tests/scanout_fixture.c \
              linuxu/tests/test_scanout_main.c; do
  object="$work/k_$(basename "$source" .c).o"
  clang "${kernel[@]}" -fsanitize=address -Ilibmlg_drm/include -c "$source" -o "$object"
  objects+=("$object")
done
# The client builds as a Mesa driver does (libdrm-mlg's headers, this
# platform's <drm.h>) into a library of its own linked against libdrm-mlg;
# the fixture helpers it calls are the program's.
prefix=build/libdrm-mlg
lib=$(cd $prefix/lib && pwd)
clang -std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1 \
  -fsanitize=address -I$prefix/include -I$prefix/include/libdrm -Ilinuxu/tests \
  -dynamiclib -install_name @rpath/libtest_scanout.dylib \
  libmlg_drm/libdrm/tests/test_scanout.c -L"$lib" -ldrm_mlg -lmlg_drm \
  -Wl,-undefined,dynamic_lookup -o "$work/libtest_scanout.dylib"
clang -fsanitize=address "${objects[@]}" build/libmacamgdu.a \
  -L"$work" -ltest_scanout -L"$lib" -lmlg_drm -Wl,-rpath,"$lib" -Wl,-rpath,"$work" \
  -framework IOKit -framework CoreFoundation -framework CoreGraphics -lpthread \
  -o "$work/test_scanout"
# SCANOUT_KEEP=<path> keeps the binary for a debugger.
if [ -n "${SCANOUT_KEEP:-}" ]; then
  cp "$work/test_scanout" "$SCANOUT_KEEP"
  cp "$work/libtest_scanout.dylib" "$(dirname "$SCANOUT_KEEP")/"
fi
"$work/test_scanout"
