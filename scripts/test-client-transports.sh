#!/usr/bin/env bash
# The driver's clients' real IOKit transports, the HSA runtime's and
# libmlg_drm's, against the dext's real dispatch
# (linuxu/tests/test_client_transports.cpp): their sources as they ship, the
# dext's ExternalMethod delivery part, lx_external_method and owner calls
# extracted from dext/sources/MacLinuxGPUXcode.mm, and a test kernel between
# them that fails on an async call the driver answers without a completion
# (build 243's LX_MMAP_COMMIT hang). The HSA runtime brings a cold GPU up and
# creates queues as LemonSeed Engine's hrx does (build 245 refused the second:
# the topology's 16 words did not fit an owner call's completion).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir" <<'PY'
import pathlib, sys
source = pathlib.Path("dext/sources/MacLinuxGPUXcode.mm").read_text()
out = pathlib.Path(sys.argv[1])
def section(start, end, lines, name):
    if source.count(start) != 1 or source.count(end) < 1:
        raise SystemExit(f"{name}: extraction markers changed")
    begin = source.index(start)
    text = source[begin:source.index(end, begin)]
    if len(text.splitlines()) > lines:
        raise SystemExit(f"{name}: extraction exceeded its bounded section")
    return text
parts = [
    section("enum {\n    kMacAMDGPUMethodPing", "static_assert(kMacAMDGPUMethodReleaseQuarantine", 70, "selectors"),
    section("static uint32_t s_lxRegistryLock;", "static void lx_gate_close()", 40, "registry"),
    section("// Linux-file calls (rt/lx_abi.h selectors)", "// Interrupt-driven waits (selectors 86 and 87", 200, "lx"),
    section("static kern_return_t lx_call(MacLinuxGPUUserClient *client", "// A Linux-file mapping as client memory", 230, "lx calls"),
    section("// Session calls off the delivery thread", "// A client's last display op result", 520, "owner calls"),
]
(out / "lx_transport_production.inc").write_text("\n".join(parts))
delivery = section("    if (reference != &kOwnerJob) {", "    // On the owner's queue (owner_job_main)", 45, "delivery")
(out / "lx_transport_delivery.inc").write_text(delivery)
PY
fake=linuxu/tests/client_transport_iokit_fake.h
sanitize=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
objects=()
for source in libmlg_drm/src/mlg_drm.c libmlg_drm/src/mlg_transport_iokit.c libmlg_drm/src/mlg_init.c \
              linuxu/src/amdgpu-rt/lx_frame.c linuxu/src/amdgpu-rt/lx_describe.c; do
  object="$test_dir/$(basename "$source" .c).o"
  clang -std=c11 -Wall -Wextra -Werror "${sanitize[@]}" -DMLG_LX_CLIENT_BUILD -include "$fake" -Ihost \
    -Ilibmlg_drm/include -Ilibmlg_drm/compat -Ilibmlg_drm/src -Ithird_party/linux/include/uapi \
    -idirafter linuxu/headers -c "$source" -o "$object"
  objects+=("$object")
done
# The HSA runtime (hsa/CMakeLists.txt's sources) and its firmware servicer.
for source in hsa/src/runtime.cpp hsa/src/gpu_signals.cpp hsa/src/gpu_signal_service.cpp hsa/src/memory.cpp \
              hsa/src/queues.cpp hsa/src/power.cpp hsa/src/device_init.cpp hsa/src/isa.cpp hsa/src/isa_target.cpp \
              hsa/src/signal_kernels.cpp hsa/src/code_object.cpp hsa/src/executable.cpp hsa/src/virtual_memory.cpp \
              hsa/src/host_services.cpp hsa/src/host_window.cpp hsa/src/allocation_census.cpp \
              hsa/src/ipc_memory.cpp hsa/src/ipc_signal.cpp hsa/src/platform_extensions.cpp \
              hsa/src/transport_iokit.cpp hsa/src/transport_fake.cpp host/fw_mailbox_service.c host/fw_mailbox_iokit.c; do
  object="$test_dir/hsa_$(basename "$source").o"
  case "$source" in
    *.c) clang -std=gnu11 -Wall -Wextra -Werror "${sanitize[@]}" -include "$fake" -Ihost -Ihsa/include \
           -Ihsa/third_party/hsa/include -c "$source" -o "$object" ;;
    *) clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -include "$fake" -DHSA_EXPORT -Ihost \
         -Ihsa/include -Ihsa/third_party/hsa/include -Ihsa/src -c "$source" -o "$object" ;;
  esac
  objects+=("$object")
done
clang++ -std=c++20 -fblocks -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
  -Wno-unused-variable -Wno-missing-field-initializers "${sanitize[@]}" -include "$fake" \
  -I"$test_dir" -Idext/sources -Ihost -Ilibmlg_drm/include -Ihsa/include -Ihsa/src -Ihsa/third_party/hsa/include -idirafter linuxu/headers \
  -c linuxu/tests/test_client_transports.cpp -o "$test_dir/test_client_transports.o"
clang++ "${sanitize[@]}" "$test_dir/test_client_transports.o" "${objects[@]}" \
  -framework IOKit -framework CoreFoundation -lpthread -o "$test_dir/test_client_transports"
[ -n "${CLIENT_TRANSPORTS_KEEP:-}" ] && cp "$test_dir/test_client_transports" "$CLIENT_TRANSPORTS_KEEP" || true
"$test_dir/test_client_transports"
# An HSA client exiting with everything loaded: status 0, no abort in exit().
"$test_dir/test_client_transports" exit-loaded
