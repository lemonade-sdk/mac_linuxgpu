#!/usr/bin/env bash
# Session open/probe/close cycles (linuxu/tests/test_session_cycles.c): the
# real upstream amdgpu probe, the production DriverKit PCI and MMIO-token
# code and the production heap adapter, against a mock IOPCIDevice. Every
# resource a session takes must return to its baseline after each close.
set -euo pipefail
cd "$(dirname "$0")/.."

make lib >/dev/null
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/linuxgpu-session-cycles.XXXXXX")
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
    # The session's PCI device, MMIO tokens and register access are the
    # DriverKit build's.
    linuxu/src/amdgpu-rt/device.c|linuxu/src/pci/pci_stub.c|linuxu/src/pci/pdev_mmio.c|third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c|third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_reg_access.c)
      extra=(-DLINUXU_DEXT_DK=1); rebuild=1 ;;
    linuxu/src/kmem/kmemcheck.c|third_party/linux/drivers/gpu/drm/drm_managed.c|third_party/linux/drivers/gpu/drm/drm_debugfs.c)
      rebuild=1 ;;
  esac
  # Every source that allocates from the C heap uses the production adapter.
  if [[ -f "$source" ]] && grep -Eq '\b(malloc|free|calloc|realloc|strdup|strndup|aligned_alloc)[[:space:]]*\(' "$source"; then
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
ar rcs "$work_dir/libmacamgdu-cycles.a" "${objects[@]}"
clang "${common[@]}" "${sanitizers[@]}" "${heap_aliases[@]}" -DLINUXU_DEXT_DK=1 \
  "${include_flags[@]}" -c linuxu/src/shims/dext_alloc.c -o "$work_dir/dext_alloc.o"
clang "${sanitizers[@]}" -c linuxu/tests/dext_heap_backend.c -o "$work_dir/heap_backend.o"
mkdir -p build/tests
clang "${common[@]}" "${sanitizers[@]}" "${heap_aliases[@]}" -DLINUXU_DEXT_DK=1 "${include_flags[@]}" \
  linuxu/tests/test_session_cycles.c "$work_dir/dext_alloc.o" "$work_dir/heap_backend.o" \
  "$work_dir/libmacamgdu-cycles.a" \
  -Wl,-dead_strip -lpthread -o build/tests/test_session_cycles
UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0 build/tests/test_session_cycles "${SESSION_CYCLES:-300}"
