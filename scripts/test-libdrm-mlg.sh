#!/usr/bin/env bash
# libdrm-mlg, the libdrm and libdrm_amdgpu API over libmlg_drm, end to end
# through the loopback transport into upstream DRM/amdgpu on the CS fixture
# device (libmlg_drm/libdrm/tests/test_libdrm_mlg.c). Links the host
# library (make lib) and the shared libdrm-mlg (make libdrm-mlg).
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
              linuxu/tests/cs_fixture.c linuxu/tests/lx_loopback.c; do
  object="$work/k_$(basename "$source" .c).o"
  clang "${kernel[@]}" -Ilibmlg_drm/include -c "$source" -o "$object"
  objects+=("$object")
done
# The client builds as a Mesa driver does (libdrm-mlg's headers, this
# platform's <drm.h>) into a library of its own linked against libdrm-mlg,
# so its libdrm_amdgpu calls cannot bind to same-named kernel functions of
# the program; the fixture helpers it calls are the program's.
prefix=build/libdrm-mlg
lib=$(cd $prefix/lib && pwd)
clang -std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1 \
  -fsanitize=address -I$prefix/include -I$prefix/include/libdrm -Ilinuxu/tests \
  -dynamiclib -install_name @rpath/libtest_libdrm_mlg.dylib \
  libmlg_drm/libdrm/tests/test_libdrm_mlg.c -L"$lib" -ldrm_mlg -lmlg_drm \
  -Wl,-undefined,dynamic_lookup -o "$work/libtest_libdrm_mlg.dylib"
clang -std=c11 -Wall -Wextra -Werror -g -O1 -Ilibmlg_drm/include -Ilinuxu/tests \
  -c libmlg_drm/libdrm/tests/test_libdrm_mlg_main.c -o "$work/main.o"
clang -fsanitize=address "${objects[@]}" "$work/main.o" build/libmacamgdu.a \
  -L"$work" -ltest_libdrm_mlg -L"$lib" -lmlg_drm -Wl,-rpath,"$lib" -Wl,-rpath,"$work" \
  -framework IOKit -framework CoreFoundation -lpthread -o "$work/test_libdrm_mlg"
"$work/test_libdrm_mlg"
