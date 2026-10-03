#!/usr/bin/env bash
# llama.cpp offline: the Vulkan backend of the pinned llama.cpp
# (scripts/build-llama-vulkan.sh) on RADV (scripts/build-radv.sh) on the CS
# fixture's software GPU. vulkan/tests/fixture_preload.c brings the fixture
# up inside the llama.cpp program (inserted by scripts/llama-radv.sh,
# MLG_PRELOAD) and installs the
# loopback transport, so the program's RADV reaches the fixture.
#
# Checks that llama.cpp finds the GPU as a Vulkan device and that
# test-backend-ops in performance mode creates its pipelines (compiled by
# ACO), allocates its buffers and submits its work with fences signaling.
# LLAMA_TEST_KEEP=<path> keeps the fixture library for manual runs:
#   MLG_PRELOAD=<path> scripts/llama-radv.sh test-backend-ops perf -o ROPE
# The software GPU runs no shaders, so nothing checks results here.
#
# Skips (exit 0) when RADV or llama.cpp has not been built.
set -euo pipefail
cd "$(dirname "$0")/.."
pkg=build/radv/install/lib
bin=build/llama.cpp/bin
if [ ! -f $pkg/libvulkan_radeon.dylib ] || [ ! -x $bin/test-backend-ops ]; then
  echo "test-llama-offline: SKIP (needs make radv and make llama-vulkan)"
  exit 0
fi
make -s lib libdrm-mlg >/dev/null
work=$(mktemp -d)
trap '[ -n "${LLAMA_TEST_KEEP:-}" ] && cp "$work/libmlg_fixture.dylib" "$LLAMA_TEST_KEEP"; rm -rf "$work"' EXIT

for dylib in libdrm_mlg.dylib libmlg_drm.dylib; do
  if ! cmp -s build/libdrm-mlg/lib/$dylib $pkg/$dylib; then
    install -m 755 build/libdrm-mlg/lib/$dylib $pkg/
    codesign --force --sign - $pkg/$dylib 2>/dev/null
  fi
done

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
kernel=("${hostcflags[@]}" -g -O1 -fPIC "${includes[@]}")
objects=()
for source in third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c \
              third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c \
              linuxu/tests/cs_fixture.c linuxu/tests/lx_loopback.c; do
  object="$work/k_$(basename "$source" .c).o"
  clang "${kernel[@]}" -Ilibmlg_drm/include -c "$source" -o "$object"
  objects+=("$object")
done
clang -std=c11 -Wall -Wextra -Werror -g -O1 -fPIC -Ilibmlg_drm/include -Ilinuxu/tests \
  -c vulkan/tests/fixture_preload.c -o "$work/fixture_preload.o"
lib=$(cd $pkg && pwd)
# The package's libmlg_drm, the image RADV's libdrm-mlg uses.
clang -dynamiclib -install_name @rpath/libmlg_fixture.dylib "${objects[@]}" \
  "$work/fixture_preload.o" build/libmacamgdu.a -L"$lib" -lmlg_drm \
  -Wl,-rpath,"$lib" -framework IOKit -framework CoreFoundation -lpthread \
  -o "$work/libmlg_fixture.dylib"

export MLG_PRELOAD="$work/libmlg_fixture.dylib"
export MESA_SHADER_CACHE_DISABLE=true
devices=$(scripts/llama-radv.sh $bin/llama-bench --list-devices 2>&1 | grep -v '^<' || true)
echo "$devices" | grep -E 'Vulkan0|Available' || true
echo "$devices" | grep -q 'Vulkan0: AMD Radeon AI Pro R9700 (RADV GFX1201)' ||
  { echo "$devices" >&2; echo "test-llama-offline: llama.cpp did not find the GPU" >&2; exit 1; }

# Performance mode runs each operation on the backend without comparing
# against the CPU: ADD, and the matrix multiplications of a Q4 model's
# feed-forward layer, one token (matrix-vector pipelines) and a 512-token
# batch (matrix-matrix pipelines).
run_perf() { # op [params regex]
  local out
  out=$(scripts/llama-radv.sh $bin/test-backend-ops perf -b Vulkan0 -o "$1" ${2:+-p "$2"} 2>&1 |
        grep -v '^<' || true)
  if ! echo "$out" | grep -q 'Backend Vulkan0: .*OK' || ! echo "$out" | grep -q '2/2 backends passed' ||
     ! echo "$out" | grep -q ' runs - '; then
    echo "$out" | tail -20 >&2
    echo "test-llama-offline: test-backend-ops perf -o $1 failed" >&2
    exit 1
  fi
  echo "    $1: $(echo "$out" | grep -c ' runs - ') shape(s) submitted"
}
run_perf ADD
run_perf MUL_MAT 'type_a=(q4_0|q4_K),type_b=f32,m=4096,n=(1|512),k=14336'
echo "PASS llama.cpp offline: Vulkan backend on RADV on the fixture; device found, pipelines" \
  "compiled by ACO, buffers allocated, ADD and Q4 MUL_MAT (vector and matrix) submitted with" \
  "fences signaling"
