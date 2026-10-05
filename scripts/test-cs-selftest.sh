#!/usr/bin/env bash
# Kernel-queue command submission offline: the CS self-test through the
# Linux-file transport against the unmodified upstream DRM core, GEM, TTM,
# VM, rings, fences, drm_sched, CS and syncobjs of a fixture device whose
# software GPU executes the rings (linuxu/tests/cs_fixture.c). Links the
# host library (make lib).
set -euo pipefail
cd "$(dirname "$0")/.."
make -s lib >/dev/null
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
cflags=("${hostcflags[@]}" -g -O1 "${includes[@]}")
# The fixture's GART table and VRAM are host memory. The library's host
# build routes readX/writeX through synthetic MMIO tokens, which would drop
# the GART PTE writes (amdgpu_gmc_set_pte_pde uses writeq); these two
# upstream files are rebuilt with plain-memory accessors, as the KFD session
# test builds its doorbell code, and take the archive members' place.
plain=()
for source in third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c \
              third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c; do
  object="$work/$(basename "$source" .c).o"
  clang "${cflags[@]}" -c "$source" -o "$object"
  plain+=("$object")
done
clang "${cflags[@]}" -fsanitize=address \
  linuxu/tests/test_cs_selftest.c linuxu/tests/cs_fixture.c linuxu/tests/ttm_evict_check.c \
  linuxu/tests/removal_check.c linuxu/tests/blocked_call_check.c \
  "${plain[@]}" build/libmacamgdu.a \
  -lpthread -o "$work/test_cs_selftest"
"$work/test_cs_selftest" wedge
# CS_SELFTEST_KEEP=<path> keeps the binary for a debugger; CS_FIXTURE_TRACE=1
# traces what the software engines execute.
if [ -n "${CS_SELFTEST_KEEP:-}" ]; then cp "$work/test_cs_selftest" "$CS_SELFTEST_KEEP"; fi
"$work/test_cs_selftest"
"$work/test_cs_selftest" wedge
