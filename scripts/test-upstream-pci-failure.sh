#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Reuse unchanged upstream objects, but compile every source that directly
# consumes libc heap memory with the production DriverKit allocation adapter.
# Namespacing its exported symbols keeps the system test process on libc while
# preserving the same allocation ownership boundaries as the installed driver.
make lib >/dev/null
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/linuxgpu-pci-probe.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
cat > "$work_dir/flags.mk" <<'MAKE'
.PHONY: print-includes print-probe-objects
print-includes:
	@printf '%s\n' $(INCPATHS)
print-probe-objects:
	@printf '%s\n' $(DRIVER_OBJS) $(LINUXU_OBJS)
MAKE
include_string=$(make -s -f Makefile -f "$work_dir/flags.mk" print-includes | tr '\n' ' ')
read -r -a include_flags <<< "$include_string"
common=(-std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0
        -include linux/autoconf.h -w -Wno-incompatible-function-pointer-types
        -ffunction-sections -fdata-sections)
sanitizers=(-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
heap_aliases=(-Dmalloc=dext_test_malloc -Dcalloc=dext_test_calloc
              -Drealloc=dext_test_realloc -Daligned_alloc=dext_test_aligned_alloc
              -Dfree=dext_test_free -Dstrdup=dext_test_strdup -Dstrndup=dext_test_strndup)
objects=()
while IFS= read -r object; do
  case "$object" in
    build/driver/amdgpu/amdgpu_discovery.o) continue ;;
    build/gen/*) source="$object" ;;
    build/linuxu/*) source="linuxu/src/${object#build/linuxu/}" ;;
    build/driver/drm-core/*) source="third_party/linux/drivers/gpu/drm/${object#build/driver/drm-core/}" ;;
    build/driver/ttm/*|build/driver/scheduler/*) source="third_party/linux/drivers/gpu/drm/${object#build/driver/}" ;;
    build/driver/*) source="third_party/linux/drivers/gpu/drm/amd/${object#build/driver/}" ;;
    *) echo "unknown probe object: $object" >&2; exit 1 ;;
  esac
  source="${source%.o}.c"
  extra=(-DLINUXU_RT_HOST_SHADOW=1)
  rebuild=0
  case "$source" in
    linuxu/src/amdgpu-rt/device.c|linuxu/src/pci/pci_stub.c|linuxu/src/pci/pdev_mmio.c|third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c)
      extra=(-DLINUXU_DEXT_DK=1); rebuild=1 ;;
    third_party/linux/drivers/gpu/drm/drm_drv.c)
      extra+=(-D__devm_drm_dev_alloc=__devm_drm_dev_alloc_impl); rebuild=1 ;;
    linuxu/src/kmem/kmemcheck.c|third_party/linux/drivers/gpu/drm/drm_managed.c|third_party/linux/drivers/gpu/drm/drm_debugfs.c)
      rebuild=1 ;;
  esac
  if [[ -f "$source" ]] && rg -q '\b(malloc|free|calloc|realloc|strdup|strndup|aligned_alloc)\s*\(' "$source"; then
    rebuild=1
  fi
  if (( rebuild )); then
    replacement="$work_dir/${object//\//_}"
    clang "${common[@]}" "${sanitizers[@]}" "${heap_aliases[@]}" "${extra[@]}" \
      "${include_flags[@]}" -c "$source" -o "$replacement"
    object="$replacement"
  fi
  objects+=("$object")
done < <(make -s -f Makefile -f "$work_dir/flags.mk" print-probe-objects)
ar rcs "$work_dir/libmacamgdu-probe.a" "${objects[@]}"
clang "${common[@]}" "${sanitizers[@]}" "${heap_aliases[@]}" -DLINUXU_DEXT_DK=1 \
  "${include_flags[@]}" -c linuxu/src/shims/dext_alloc.c -o "$work_dir/dext_alloc.o"
clang "${sanitizers[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work_dir/heap_backend.o"
mkdir -p build/tests
clang "${common[@]}" "${sanitizers[@]}" "${heap_aliases[@]}" -DLINUXU_DEXT_DK=1 "${include_flags[@]}" \
  linuxu/tests/test_upstream_pci_failure.c "$work_dir/dext_alloc.o" "$work_dir/heap_backend.o" \
  "$work_dir/libmacamgdu-probe.a" \
  -Wl,-dead_strip -lpthread -o build/tests/test_upstream_pci_failure
UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 build/tests/test_upstream_pci_failure --bars

# Each allocation budget runs in a fresh userspace process. A failure cannot
# inherit initialized module globals from the preceding case.
mkdir -p build/diagnostics
oom_log=build/diagnostics/upstream-probe-allocation-failures.log
: > "$oom_log"
for ((fail_after = 0; fail_after < 64; fail_after++)); do
  if ! UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 build/tests/test_upstream_pci_failure \
      --probe-oom "$fail_after" >> "$oom_log" 2>&1; then
    tail -80 "$oom_log" >&2
    echo "error: upstream probe allocation budget $fail_after failed" >&2
    exit 1
  fi
done
echo "production heap upstream probe passed 64 allocation budgets"

# Reproduce the old no-op kset registration in this userspace-only process.
# A passing control would mean the regression check missed the original fault.
mkdir -p build/diagnostics
control_log=build/diagnostics/discovery-legacy-kset.log
if UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 build/tests/test_upstream_pci_failure \
    --discovery-legacy-kset > "$control_log" 2>&1; then
  echo "error: legacy kset control unexpectedly survived cleanup" >&2
  exit 1
fi
if ! rg -q 'amdgpu_discovery.c:1403:.*(runtime error|member access)' "$control_log"; then
  cat "$control_log" >&2
  echo "error: legacy kset control failed outside the expected cleanup access" >&2
  exit 1
fi
echo "legacy kset control reproduced the upstream discovery cleanup fault"
