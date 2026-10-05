#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir" <<'PY'
import pathlib, sys
source = pathlib.Path("dext/sources/MacLinuxGPUXcode.mm").read_text()
def section(start, end, limit):
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit("shutdown fixture extraction markers changed")
    begin = source.index(start)
    finish = source.index(end, begin)
    text = source[begin:finish]
    if len(text.splitlines()) > limit:
        raise SystemExit("shutdown fixture extraction exceeded its bounded section")
    return text
state = section("static IODispatchQueue *s_bringupQueue", "class ComputeClientScope", 130)
close = section("static void session_irq_drained(void *context)", "static kern_return_t ensure_open", 600)
opening = section("static kern_return_t ensure_open(MacLinuxGPUUserClient *client)", "static kern_return_t prepare_interrupts", 50)
driver_stop = section("kern_return_t\nIMPL(MacLinuxGPU, Stop)", "void\nMacLinuxGPU::FinishSession()", 50)
finish = section("void\nMacLinuxGPU::FinishSession()", "void\nMacLinuxGPU::FinishStop(IOService *provider)", 100)
lx_finish = section("static bool session_leave_closes(bool participant, bool legacyClient);\n", "// The client's process exits (async calls return", 40)
client_stop = section("kern_return_t\nIMPL(MacLinuxGPUUserClient, Stop)", "void\nMacLinuxGPUUserClient::FinishStop(IOService *provider)", 120)
shutdown = section("    case kMacAMDGPUMethodShutdownGPU: {", "    case kMacAMDGPUMethodGetReBARInfo: {", 40)
probe_failure = section("        if (s_probeResult != 0) {", '        MACLINUXGPU_EVENT("upstream AMDGPU PCI probe completed");', 20)
wrapper = """
kern_return_t MacLinuxGPUUserClient::shutdown(IOUserClientMethodArguments *arguments)
{
    uint64_t *out = arguments->scalarOutput;
    switch (kMacAMDGPUMethodShutdownGPU) {
""" + shutdown + """
    }
    return kIOReturnError;
}
"""
probe_wrapper = """
kern_return_t MacLinuxGPUUserClient::failedProbe()
{
""" + probe_failure + """
    return kIOReturnSuccess;
}
"""
pathlib.Path(sys.argv[1], "session_shutdown_production.inc").write_text(state + close + opening + driver_stop + finish + lx_finish + client_stop + wrapper + probe_wrapper)
PY
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections \
  -Ilinuxu/headers -c linuxu/src/shims/printk.c -o "$test_dir/printk.o"
clang++ -std=c++20 -fblocks -Wall -Wextra -Werror -Wno-unused-variable \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$test_dir" -Idext/sources -idirafter linuxu/headers \
  linuxu/tests/test_session_shutdown.cpp "$test_dir/printk.o" \
  -Wl,-dead_strip -o "$test_dir/test_session_shutdown"
for scenario in log-format success large-mapped hold-failure compute-failure irq-failure irq-failure-late \
  reset-failure dma-fini-failure pre-quarantined isolation-failure raw-mapped shutdown-selector probe-retained \
  client-exit-reopen queue-exhaustion-exit client-exit-unprobed client-exit-release-failure \
  client-exit-raw-mapped client-exit-legacy concurrent-clients client-churn last-leaver-churn transport-fault-removal observer-quarantined release-after-isolation-failure \
  release-refused-upstream release-reset-failed stop-release pci-fault-cause observer-reads \
  selftest-parked display-showing display-quarantined surprise-removal surprise-removal-quarantined surprise-removal-held \
  upgrade-stop-idle upgrade-stop-session upgrade-stop-quarantined upgrade-stop-quarantined-held \
  retire-idle retire-quiesce-resume retire-session retire-raw-bar retire-quarantined retire-quarantined-held; do
  "$test_dir/test_session_shutdown" "$scenario"
done
