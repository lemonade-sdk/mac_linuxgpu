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
export UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0
build/tests/test_session_cycles "${SESSION_CYCLES:-300}"

# The deep mode: the card's IP discovery binary and VBIOS take the probe past
# IP discovery into the IPs' initialization (linuxu/tests/test_session_cycles.c).
# deep_run FIXTURES CYCLES: the sizes it needs come from the capture's JSON.
deep_run() {
  local args
  args=$(python3 - "$1/ip_discovery.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
bars = d["bar_sizes"]
print(d["vram_total_bytes"] >> 20, bars["0"], bars["2"], bars["5"])
PY
)
  # shellcheck disable=SC2086
  build/tests/test_session_cycles --deep "$1" build/firmware/amdgpu $args "$2"
}

# Self-check of the deep harness on every run (CI included), with a synthetic
# description shaped like an RDNA4 card (made-up base addresses) and a ROM
# that has a valid header but no ATOM tables: upstream must read the
# discovery binary, find SMUIO, read the whole ROM through it and then refuse
# it, the same way every cycle, with every resource back at its baseline.
synthetic="$work_dir/synthetic"
mkdir -p "$synthetic"
python3 - "$synthetic" <<'PY'
import json, os, sys
sys.path.insert(0, "scripts")
import ip_discovery_builder as b
out = sys.argv[1]
ips = []
def ip(hw_id, version, instance=0, bases=None):
    base = 0x1000 + 0x400 * len(ips)
    ips.append({"hw_id": hw_id, "instance": instance, "major": version[0], "minor": version[1],
                "revision": version[2], "harvest": 0,
                "base_addresses": bases or [base, base + 0x100, base + 0x200]})
ip(11, (12, 0, 1))                    # GC
ip(42, (7, 0, 1)); ip(42, (7, 0, 1), 1)  # SDMA0, two instances
ip(34, (4, 1, 0)); ip(35, (4, 1, 0))  # MMHUB, ATHUB
ip(108, (6, 3, 1))                    # NBIF
ip(255, (14, 0, 3)); ip(1, (14, 0, 3))  # MP0, MP1
ip(40, (7, 0, 0)); ip(41, (7, 0, 0))  # OSSSYS, HDP
ip(4, (14, 0, 2))                     # SMUIO
ip(271, (4, 0, 1))                    # DMU
ip(12, (5, 0, 0))                     # VCN
desc = {"format": 1, "dies": [{"die_id": 0, "ips": ips}],
        "gc_info": {"num_se": 4, "num_wgp0_per_sa": 4, "num_wgp1_per_sa": 0, "num_rb_per_se": 4,
                    "num_gl2c": 16, "num_gprs": 1536, "num_max_gs_thds": 32, "gs_table_depth": 32,
                    "gsprim_buff_depth": 1792},
        "vram_total_bytes": 16 << 30,
        "bar_sizes": {"0": 256 << 20, "2": 2 << 20, "5": 512 << 10}}
json.dump(desc, open(os.path.join(out, "ip_discovery.json"), "w"))
open(os.path.join(out, "ip_discovery.bin"), "wb").write(b.build(desc))
rom = bytearray(64 << 10)
rom[0:2] = b"\x55\xaa"
rom[2] = len(rom) // 512                    # AMD_VBIOS_LENGTH
rom[0x30:0x3a] = b" 761295520"             # AMD_VBIOS_SIGNATURE
open(os.path.join(out, "vbios.rom"), "wb").write(rom)
PY
deep_run "$synthetic" 20

fixtures=${R9700_FIXTURES:-tests/fixtures/local/r9700}
if [[ -f "$fixtures/ip_discovery.bin" && -f "$fixtures/vbios.rom" && -f "$fixtures/ip_discovery.json" ]]; then
  deep_run "$fixtures" "${SESSION_CYCLES_DEEP:-200}"
else
  echo "SKIP deep session cycles: no card capture in $fixtures (ip_discovery.json, ip_discovery.bin," \
       "vbios.rom); scripts/capture-r9700-fixtures.py records them from a running driver," \
       "see tests/fixtures/local/README.md"
fi
