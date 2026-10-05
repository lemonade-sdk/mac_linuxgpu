#!/usr/bin/env bash
# libmlg_drm, the Linux-file RPC client library: on its own against a
# recording transport, then end to end through the Linux-file core into
# upstream DRM/amdgpu on the CS fixture device (links the host library).
set -euo pipefail
cd "$(dirname "$0")/.."
make -s lib >/dev/null
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
client=(-std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -g -O1
  -DMLG_LX_CLIENT_BUILD -Ihost -Ilibmlg_drm/include -Ilibmlg_drm/compat -Ilibmlg_drm/src
  -Ithird_party/linux/include/uapi -idirafter linuxu/headers)
client_srcs=(libmlg_drm/src/mlg_drm.c libmlg_drm/src/mlg_transport_iokit.c libmlg_drm/src/mlg_init.c
  linuxu/src/amdgpu-rt/lx_frame.c linuxu/src/amdgpu-rt/lx_describe.c)
frameworks=(-framework IOKit -framework CoreFoundation)

clang "${client[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
  libmlg_drm/tests/test_mlg_drm.c "${client_srcs[@]}" "${frameworks[@]}" -o "$work/test_mlg_drm"
"$work/test_mlg_drm"
# Bringing the GPU up from a Linux-file client: host window, then InitDevice.
clang "${client[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
  libmlg_drm/tests/test_mlg_init.c libmlg_drm/src/mlg_init.c -o "$work/test_mlg_init"
"$work/test_mlg_init"

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
for source in libmlg_drm/tests/test_mlg_drm_cs.c "${client_srcs[@]}"; do
  object="$work/c_$(basename "$source" .c).o"
  clang "${client[@]}" -fsanitize=address -Ilinuxu/tests -c "$source" -o "$object"
  objects+=("$object")
done
clang -fsanitize=address "${objects[@]}" build/libmacamgdu.a "${frameworks[@]}" -lpthread \
  -o "$work/test_mlg_drm_cs"
"$work/test_mlg_drm_cs"
