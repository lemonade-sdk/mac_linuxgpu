#!/usr/bin/env bash
# libmlg_drm's real IOKit transport against the dext's real Linux-file
# dispatch (linuxu/tests/test_lx_transport.cpp): the library's sources as
# they ship, the dext's ExternalMethod delivery part, lx_external_method and
# owner calls extracted from dext/sources/MacLinuxGPUXcode.mm, and a test
# kernel between them that fails on an async call the driver answers
# without a completion (build 243's LX_MMAP_COMMIT hang).
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
    section("// Session calls off the delivery thread", "// A client's last display op result", 480, "owner calls"),
]
(out / "lx_transport_production.inc").write_text("\n".join(parts))
delivery = section("    if (reference != &kOwnerJob) {", "    // On the owner's queue (owner_job_main)", 40, "delivery")
(out / "lx_transport_delivery.inc").write_text(delivery)
PY
fake=linuxu/tests/lx_transport_iokit_fake.h
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
clang++ -std=c++20 -fblocks -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
  -Wno-unused-variable -Wno-missing-field-initializers "${sanitize[@]}" -include "$fake" \
  -I"$test_dir" -Idext/sources -Ilibmlg_drm/include -idirafter linuxu/headers \
  -c linuxu/tests/test_lx_transport.cpp -o "$test_dir/test_lx_transport.o"
clang++ "${sanitize[@]}" "$test_dir/test_lx_transport.o" "${objects[@]}" \
  -framework IOKit -framework CoreFoundation -lpthread -o "$test_dir/test_lx_transport"
"$test_dir/test_lx_transport"
