#!/usr/bin/env python3
"""Read cached probe status and retained shim logs without initializing a GPU."""
import argparse
import ctypes as c
import json
import sys


def read_snapshot(query, cursor, report=lambda message: None):
    """Return one bounded cached-log snapshot and its first unread byte offset."""
    if not 0 <= cursor <= (1 << 64) - 1:
        raise RuntimeError("cursor must be an unsigned 64-bit integer")
    target = None
    collected = bytearray()
    for _ in range(512):
        values, count = query([0x4c4c4f47, cursor], 16)
        if not 3 <= count <= 16 or len(values) < count:
            raise RuntimeError("invalid log snapshot")
        if any(not 0 <= value <= (1 << 64) - 1 for value in values[:count]):
            raise RuntimeError("invalid log scalar")
        end, next_cursor, size = values[:3]
        if size > (count - 3) * 8 or size > 104 or size > next_cursor:
            raise RuntimeError("invalid log byte count")
        start = next_cursor - size
        if end < next_cursor or (start < cursor <= end) or (not size and next_cursor != end):
            raise RuntimeError("invalid log cursor")
        if target is None:
            target = end
        if start != cursor:
            report(f"log cursor clamped: {cursor} -> {start}")
        data = b"".join(value.to_bytes(8, "little") for value in values[3:count])[:size]
        collected.extend(data[:max(0, target - start)])
        # A producer can append between reads. Bytes beyond the first end
        # belong to the next snapshot, including when this chunk contains them.
        cursor = min(next_cursor, target)
        if next_cursor >= target or not size:
            return bytes(collected), cursor
    raise RuntimeError("cached snapshot exceeded its bounded read")


SESSION_STATE_TAG = 0x4c534553
SESSION_STATE_WORDS = 9
OBSERVER_CLIENT = 1
UNSUPPORTED = 0xe00002c7
# Names follow dext/sources/session_state.h.
SESSION_FLAGS = ["closing", "quarantined", "stopping", "pci_open", "modules_running",
                 "final_cleanup", "releasable", "restart_required", "raw_bar_mapped",
                 "runtime_device", "isolation_attempted", "device_removed", "retiring", "gpu_wedged"]
QUARANTINE_CAUSES = ["none", "raw BAR mapping lifetime uncertain", "DMA shutdown reservation failed",
                     "GPU completion uncertain (compute stop)", "interrupt cancellation failed",
                     "endpoint isolation failed", "DMA backing retained at fini", "definite PCI fault",
                     "probe DMA reservation failed", "failed-probe ownership retained",
                     "probe DMA commit failed", "shared-session client cleanup failed",
                     "release attempt failed"]
RELEASE_BLOCKERS = ["ready", "not quarantined", "interrupt drain pending", "interrupt cancellation failed",
                    "upstream driver or runtime device retained", "compute work retained",
                    "raw BAR mapping held", "session clients still attached", "DMA backing still owned",
                    "definite PCI fault", "PCI admission busy", "endpoint reset failed during release"]


def signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def describe_session(values, count):
    """Decode the cached session-state snapshot; return (fields, advice or None)."""
    if count != SESSION_STATE_WORDS or len(values) < count or values[0] != 1:
        raise RuntimeError("invalid session snapshot")
    if any(not 0 <= value <= (1 << 64) - 1 for value in values[:count]):
        raise RuntimeError("invalid session scalar")
    flags = {name: bool(values[1] >> bit & 1) for bit, name in enumerate(SESSION_FLAGS)}
    def name(table, value):
        return table[value] if value < len(table) else f"unknown ({value})"
    fields = {"flags": [key for key, value in flags.items() if value],
              "quarantine_cause": name(QUARANTINE_CAUSES, values[2]),
              "cause_code": signed(values[3]),
              "observed_by": name(QUARANTINE_CAUSES, values[4]),
              "isolation_result": signed(values[5]) if flags["isolation_attempted"] else None,
              "release_blocker": name(RELEASE_BLOCKERS, values[6]),
              "generation": values[7], "participants": values[8]}
    advice = None
    if flags["gpu_wedged"]:
        advice = ("the GPU stopped answering and its queue reset failed: every request fails; "
                  "power-cycle the GPU, then reconnect it")
    elif flags["restart_required"]:
        advice = "restart required, do not kill the driver: killing it while it holds the GPU panics macOS"
    elif flags["releasable"]:
        advice = ("session quarantined but quiescent: release it with "
                  "'MacLinuxGPUHostApp release', or deactivate the extension; do not kill the driver")
    elif flags["quarantined"]:
        advice = f"session quarantined, release pending ({fields['release_blocker']}); do not kill the driver"
    return fields, advice


# Lines that belong to display bring-up and the display test: the display
# opt-in, DMCUB load and DMUB, Display Core and its clock manager, the
# connectors, HPD, EDID, link training, commits and their waits.
DISPLAY_MARKERS = ("display", "dmub", "dmcub", "dc_", "dcn", "dce", "clk_mgr", "smu", "hpd",
                   "edid", "link training", "link_training", "flip_done", "vblank", "hw_done",
                   "dp-", "hdmi", "dvi", "connector", "crtc", "psr", "dsc", "atomic", "otg",
                   "hubp", "reg_wait", "i2c", "aux")


def display_lines(text):
    """The display-related lines of the retained log, in order."""
    return [line for line in text.splitlines()
            if any(marker in line.lower() for marker in DISPLAY_MARKERS)]

POWER_STATE_TAG = 0x4c505752
POWER_STATE_WORDS = 12
# Names follow dext/sources/power_state.h.
POWER_STATES = ["active", "suspending", "suspended", "resuming", "lost"]
POWER_FLAGS = ["vram_preserved", "system_sleep", "device_low", "client_hold", "kfd_quiesced",
               "session_closed", "ack_pending", "link_down"]
POWER_CAUSES = ["none", "client prepare", "client resume", "holding client closed", "system sleep",
                "system wake", "device low power", "device on", "KFD suspend failed",
                "KFD resume failed", "device gone after wake", "re-probed", "session closed",
                "device removed (unplugged)"]


def describe_power(values, count):
    """Decode the cached power-state snapshot; return (fields, advice or None)."""
    if count != POWER_STATE_WORDS or len(values) < count or values[0] != 1:
        raise RuntimeError("invalid power snapshot")
    if any(not 0 <= value <= (1 << 64) - 1 for value in values[:count]):
        raise RuntimeError("invalid power scalar")
    def name(table, value):
        return table[value] if value < len(table) else f"unknown ({value})"
    fields = {"power_state": name(POWER_STATES, values[1]), "power_generation": values[2],
              "power_flags": [flag for bit, flag in enumerate(POWER_FLAGS) if values[3] >> bit & 1],
              "power_cause": name(POWER_CAUSES, values[4]), "power_error": signed(values[5]),
              "low_power_holds": values[6], "quiesces": values[7], "memory_losses": values[8],
              "last_transition_us": values[9]}
    advice = None
    if values[1] == 4:
        advice = "device memory was lost (host sleep or a failed transition): clients reload; the next one re-probes"
    elif values[1] == 2:
        advice = "suspended: GPU work waits for resume" + (
            " (VRAM kept)" if values[3] & 1 else "")
    return fields, advice


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cursor", type=int, default=0, help="resume at this log byte offset")
    parser.add_argument("--display", action="store_true",
                        help="print only display bring-up and display-test lines")
    args = parser.parse_args()
    if not 0 <= args.cursor <= (1 << 64) - 1:
        parser.error("cursor must be an unsigned 64-bit integer")

    io = c.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
    system = c.CDLL("/usr/lib/libSystem.B.dylib")
    io.IOServiceNameMatching.argtypes = [c.c_char_p]
    io.IOServiceNameMatching.restype = c.c_void_p
    io.IOServiceGetMatchingService.argtypes = [c.c_uint, c.c_void_p]
    io.IOServiceGetMatchingService.restype = c.c_uint
    io.IOServiceOpen.argtypes = [c.c_uint, c.c_uint, c.c_uint, c.POINTER(c.c_uint)]
    io.IOServiceOpen.restype = c.c_int
    io.IOObjectRelease.argtypes = [c.c_uint]
    io.IOServiceClose.argtypes = [c.c_uint]
    io.IOServiceClose.restype = c.c_int
    io.IOConnectCallScalarMethod.argtypes = [c.c_uint, c.c_uint,
        c.POINTER(c.c_uint64), c.c_uint, c.POINTER(c.c_uint64), c.POINTER(c.c_uint)]
    io.IOConnectCallScalarMethod.restype = c.c_int

    service = io.IOServiceGetMatchingService(0, io.IOServiceNameMatching(b"MacLinuxGPU"))
    if not service:
        raise RuntimeError("MacLinuxGPU registry service was not found")
    port = c.c_uint()
    try:
        task = c.c_uint.in_dll(system, "mach_task_self_").value
        # Observer clients attach even while a session closes or is
        # quarantined; drivers without them reject the type as unsupported.
        result = io.IOServiceOpen(service, task, OBSERVER_CLIENT, c.byref(port))
        if (result & 0xffffffff) == UNSUPPORTED:
            result = io.IOServiceOpen(service, task, 0, c.byref(port))
    finally:
        io.IOObjectRelease(service)
    if result:
        raise RuntimeError(f"observer open failed: {result & 0xffffffff:#x}")

    def query(values, capacity):
        inputs = (c.c_uint64 * len(values))(*values)
        outputs = (c.c_uint64 * capacity)()
        count = c.c_uint(capacity)
        # Selector 21 only. These tags read cached state and never claim PCI.
        result = io.IOConnectCallScalarMethod(port, 21, inputs, len(values), outputs, c.byref(count))
        if result:
            raise RuntimeError(f"cached query {values[0]:#x} failed: {result & 0xffffffff:#x}")
        if count.value > capacity:
            raise RuntimeError("invalid cached query output size")
        return outputs, count.value

    try:
        probe, count = query([0x4c50524f], 5)
        if count != 5:
            raise RuntimeError("invalid probe snapshot")
        print(json.dumps({"attempted": probe[0], "modules_running": probe[1],
                          "probe_result": c.c_int64(probe[2]).value,
                          "transport_fault": probe[3], "fault_offset": probe[4]}),
              file=sys.stderr)
        try:
            fields, advice = describe_session(*query([SESSION_STATE_TAG], SESSION_STATE_WORDS))
        except RuntimeError as error:
            # Older drivers have no session snapshot; the log is still readable.
            print(f"session state unavailable: {error}", file=sys.stderr)
        else:
            print(json.dumps(fields), file=sys.stderr)
            if advice:
                print(advice, file=sys.stderr)
        try:
            fields, advice = describe_power(*query([POWER_STATE_TAG], POWER_STATE_WORDS))
        except RuntimeError as error:
            # Drivers before the power state decline the tag.
            print(f"power state unavailable: {error}", file=sys.stderr)
        else:
            print(json.dumps(fields), file=sys.stderr)
            if advice:
                print(advice, file=sys.stderr)
        collected, cursor = read_snapshot(query, args.cursor,
            lambda message: print(message, file=sys.stderr))
        print(f"next cursor: {cursor}", file=sys.stderr)
        if args.display:
            collected = "".join(line + "\n" for line in
                                display_lines(collected.decode(errors="replace"))).encode()
        sys.stdout.buffer.write(collected)
        sys.stdout.buffer.flush()
    finally:
        result = io.IOServiceClose(port)
        if result:
            raise RuntimeError(f"observer close failed: {result & 0xffffffff:#x}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
