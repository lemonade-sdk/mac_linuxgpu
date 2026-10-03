#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
sanitize=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -Ihsa/src -Ihsa/third_party/hsa/include \
  hsa/src/device_init.cpp hsa/src/isa_target.cpp hsa/tests/test_shim_init.cpp -o "$test_dir/test_shim_init"
"$test_dir/test_shim_init"
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -Ihsa/src -Ihsa/third_party/hsa/include \
  hsa/tests/test_shim_init_diagnostics.cpp -o "$test_dir/test_shim_init_diagnostics"
"$test_dir/test_shim_init_diagnostics"
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -ffunction-sections -fdata-sections \
  -Ihsa/src -Ihsa/third_party/hsa/include -Ihost \
  hsa/tests/test_shim_init_transport.cpp hsa/src/device_init.cpp hsa/src/isa_target.cpp \
  -framework IOKit -framework CoreFoundation -Wl,-dead_strip \
  -o "$test_dir/test_shim_init_transport"
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -ffunction-sections -fdata-sections \
  -Ihsa/src -Ihsa/third_party/hsa/include -Ihost \
  hsa/tests/test_transport_dispatch.cpp hsa/src/device_init.cpp hsa/src/isa_target.cpp \
  -framework IOKit -framework CoreFoundation -Wl,-dead_strip \
  -o "$test_dir/test_transport_dispatch"
"$test_dir/test_transport_dispatch"
"$test_dir/test_shim_init_transport"

# On-demand firmware end to end: the production transport, the production
# client servicer (host/) and the production dext-side mailbox (linuxu/src/fw).
# Only the IOKit/Mach boundaries are replaced; the mapping calls in
# fw_mailbox_iokit.c are redirected to the test's in-process mailbox.
kernel=(-w -std=gnu11 "${sanitize[@]}" -D__KERNEL__ -include linux/autoconf.h
  -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include -Ilinuxu/src)
clang "${kernel[@]}" -c linuxu/src/fw/fw_mailbox.c -o "$test_dir/fw_mailbox.o"
clang "${kernel[@]}" -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang -std=gnu11 -Wall -Wextra -Werror "${sanitize[@]}" -Ihost \
  -c host/fw_mailbox_service.c -o "$test_dir/fw_mailbox_service.o"
clang -std=gnu11 -Wall -Wextra -Werror "${sanitize[@]}" -Ihost \
  -DIOConnectMapMemory64=test_map_memory -DIOConnectUnmapMemory64=test_unmap_memory \
  -c host/fw_mailbox_iokit.c -o "$test_dir/fw_mailbox_iokit.o"
clang++ -std=c++20 -Wall -Wextra -Werror "${sanitize[@]}" -ffunction-sections -fdata-sections \
  -Ihsa/src -Ihsa/third_party/hsa/include -Ihost -Ilinuxu/src \
  hsa/tests/test_shim_init_firmware.cpp hsa/src/device_init.cpp hsa/src/isa_target.cpp \
  "$test_dir/fw_mailbox.o" "$test_dir/printk.o" "$test_dir/fw_mailbox_service.o" \
  "$test_dir/fw_mailbox_iokit.o" -framework IOKit -framework CoreFoundation -Wl,-dead_strip \
  -o "$test_dir/test_shim_init_firmware"
TMPDIR="$test_dir" "$test_dir/test_shim_init_firmware"
