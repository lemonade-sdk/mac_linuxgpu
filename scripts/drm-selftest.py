#!/usr/bin/env python3
"""Run the kernel-queue command submission self-test on the GPU.

The MacLinuxGPU driver runs, in a Linux process of its own, what a Vulkan
driver does on a render node: AMDGPU_INFO, a context, GTT and VRAM buffers
mapped in its own GPUVM, AMDGPU_CS of a PM4 IB on the compute ring and of
SDMA IBs (fill, then a copy ordered by a syncobj), AMDGPU_WAIT_CS on an
async worker and a syncobj wait, then moves its VRAM buffer to GTT and back
the way a client's submission moves it (TTM's move into GTT goes through a
GART transfer window, as an eviction does: SDMA uploads the window PTEs,
flushes VMID 0 and copies), checking every value, then undoing all of it. It uses the observer client: it never initializes the GPU, joins a
session, creates a queue or touches another client's memory, and each wait
is bounded. The GPU must already be running (a session client initialized
it); otherwise the call fails with "not ready". With --init the script
initializes it itself through a session client (InitDevice), which stays
open for the test; if no other client uses the GPU, closing it at the end
closes the session (upstream removal, function reset), as any last client
does.

  drm-selftest.py            # run once, print each step
  drm-selftest.py --init     # bring the GPU up first, then run
  drm-selftest.py --json     # the result as JSON
  drm-selftest.py --runs 5   # repeat

Exit status 0 when every step that applies passed.
"""
import argparse
import ctypes as c
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mlg_owner_call import OwnerCall  # noqa: E402

SESSION_CLIENT, OBSERVER_CLIENT = 0, 1
RUNTIME_BUILD, INIT_DEVICE, HOST_WINDOW = 43, 9, 54
DRM_SELFTEST = 82              # dext/sources/session_state.h MLG_SELECTOR_DRM_SELFTEST
CONFIRM = 0x43535354           # MLG_DRM_SELFTEST_CONFIRM ("CSST")
RESULT_MAX = 512               # MLG_DRM_SELFTEST_RESULT_MAX
NOT_READY = 0xe00002d8
NOT_PERMITTED = 0xe00002e2
BUSY = 0xe00002d5

# linuxu/headers/rt/cs_selftest.h. Version 2 adds the TTM steps and fields;
# a version 1 driver (no TTM steps) is still read.
VERSION = 2
STEPS_V1 = ["open", "version", "dev_info", "hw_ip", "ctx", "syncobj", "gem_create", "gem_va",
            "gem_mmap", "compute_cs", "compute_wait_cs", "compute_syncobj", "compute_result",
            "sdma_fill", "sdma_copy", "sdma_wait", "sdma_result", "teardown"]
STEPS = STEPS_V1[:-1] + ["ttm_gtt", "ttm_vram", "teardown"]
STATUS = {0: "passed", 1: "not run", 2: "skipped (no such engine)",
          3: "mismatch (the GPU wrote something else)",
          4: "parked (GPU work still running; the process is kept until it completes)"}
# struct rt_cs_selftest_result
LAYOUT_V1 = struct.Struct("<4I24i6I6Q4I")
LAYOUT = struct.Struct("<4I24i6I6Q4I4Q2I")
FIELDS_V1 = ["family", "chip_external_rev", "device_id", "num_shader_engines",
             "compute_rings", "sdma_rings", "va_start", "va_end", "compute_seq", "sdma_seq",
             "compute_ns", "sdma_ns", "compute_value", "vram_value", "fill_value", "user_fence"]
FIELDS = FIELDS_V1 + ["gtt_moved", "vram_moved", "gtt_ns", "vram_ns", "gtt_value",
                      "vram_back_value"]


class DriverError(RuntimeError):
    pass


def signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def describe(status):
    if status < 0:
        code = -status
        name = os.strerror(code) if 0 < code < 200 else "error"
        return f"failed: Linux errno {code} ({name})"
    return STATUS.get(status, f"status {status}")


def decode(blob):
    """The driver's struct rt_cs_selftest_result as a dict."""
    if len(blob) < LAYOUT_V1.size:
        raise DriverError(f"self-test result is {len(blob)} bytes, expected {LAYOUT.size}")
    version, steps = struct.unpack_from("<2I", blob)
    known = {1: (LAYOUT_V1, STEPS_V1, FIELDS_V1), 2: (LAYOUT, STEPS, FIELDS)}
    if version not in known or steps != len(known[version][1]):
        raise DriverError(f"self-test result version {version} with {steps} steps is not this reader's")
    layout, names, fields = known[version]
    if len(blob) < layout.size:
        raise DriverError(f"self-test result is {len(blob)} bytes, expected {layout.size}")
    values = layout.unpack_from(blob)
    passed, failed = values[2:4]
    status = values[4:4 + 24]
    result = {"version": version, "passed": passed,
              "failed_step": names[failed] if failed < len(names) else None,
              "steps": {name: status[i] for i, name in enumerate(names)}}
    result.update(zip(fields, values[28:]))
    return result


def run(call):
    """One self-test: (status, parked tests, result dict)."""
    values, blob = call(DRM_SELFTEST, [CONFIRM], RESULT_MAX)
    if len(values) < 2:
        raise DriverError("self-test reply without status")
    return signed(values[0]), values[1], decode(blob)


def report(status, parked, result, out=sys.stdout):
    for name in result["steps"]:
        print(f"  {name:<16} {describe(result['steps'][name])}", file=out)
    print(f"  device {result['device_id']:#06x} family {result['family']} rev {result['chip_external_rev']:#x}, "
          f"compute rings {result['compute_rings']:#x}, SDMA rings {result['sdma_rings']:#x}", file=out)
    print(f"  compute: seq {result['compute_seq']}, wrote {result['compute_value']:#010x}, "
          f"user fence {result['user_fence']}, {result['compute_ns'] / 1e6:.3f} ms", file=out)
    print(f"  SDMA: seq {result['sdma_seq']}, VRAM {result['vram_value']:#010x}, "
          f"fill {result['fill_value']:#010x}, {result['sdma_ns'] / 1e6:.3f} ms", file=out)
    if "gtt_moved" in result:
        print(f"  TTM: to GTT moved {result['gtt_moved']} bytes, read {result['gtt_value']:#010x}, "
              f"{result['gtt_ns'] / 1e6:.3f} ms; back to VRAM moved {result['vram_moved']} bytes, "
              f"read {result['vram_back_value']:#010x}, {result['vram_ns'] / 1e6:.3f} ms", file=out)
    print(f"  GPU VA {result['va_start']:#x}-{result['va_end']:#x}", file=out)
    verdict = "PASS" if status == 0 else f"FAIL at {result['failed_step']}: {describe(status)}"
    print(verdict, file=out)
    if parked:
        print(f"  {parked} self-test process(es) still wait for their GPU work", file=out)


def connect(init=False):
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
    io.IOConnectCallMethod.argtypes = [c.c_uint, c.c_uint, c.POINTER(c.c_uint64), c.c_uint,
                                       c.c_void_p, c.c_size_t, c.POINTER(c.c_uint64),
                                       c.POINTER(c.c_uint), c.c_void_p, c.POINTER(c.c_size_t)]
    io.IOConnectCallMethod.restype = c.c_int
    io.IOConnectCallScalarMethod.argtypes = [c.c_uint, c.c_uint, c.POINTER(c.c_uint64), c.c_uint,
                                             c.POINTER(c.c_uint64), c.POINTER(c.c_uint)]
    io.IOConnectCallScalarMethod.restype = c.c_int
    service = io.IOServiceGetMatchingService(0, io.IOServiceNameMatching(b"MacLinuxGPU"))
    if not service:
        raise DriverError("MacLinuxGPU registry service was not found")
    port = c.c_uint()
    session = c.c_uint()
    try:
        task = c.c_uint.in_dll(system, "mach_task_self_").value
        if init:
            result = io.IOServiceOpen(service, task, SESSION_CLIENT, c.byref(session))
            if result:
                raise DriverError(f"session open failed: {result & 0xffffffff:#x}")
        result = io.IOServiceOpen(service, task, OBSERVER_CLIENT, c.byref(port))
    finally:
        io.IOObjectRelease(service)
    if result:
        raise DriverError(f"observer open failed: {result & 0xffffffff:#x}")
    # Every selector that can sleep is an async session call (session_state.h).
    owner = OwnerCall(session.value) if init else None
    observer = OwnerCall(port.value)
    if init:
        build = (c.c_uint64 * 3)()
        count = c.c_uint(3)
        result = io.IOConnectCallScalarMethod(session, RUNTIME_BUILD, None, 0, build, c.byref(count))
        reservation = None
        if not result:
            # Place the host window before the probe, as the HSA runtime does:
            # query the GART aperture size, reserve that much address space
            # aligned to its size, and hand the base to the driver.
            result, window, _ = owner(HOST_WINDOW, [0], outputs=3)
            size = window[1] if len(window) > 1 else 0
            if not result and size and not size & (size - 1):
                system.mmap.argtypes = [c.c_void_p, c.c_size_t, c.c_int, c.c_int, c.c_int, c.c_long]
                system.mmap.restype = c.c_void_p
                system.munmap.argtypes = [c.c_void_p, c.c_size_t]
                raw = system.mmap(None, size * 2, 0, 0x0002 | 0x1000, -1, 0)  # PROT_NONE, MAP_PRIVATE|MAP_ANON
                if raw and raw != c.c_void_p(-1).value:
                    reservation = (raw, size * 2)
                    base = (raw + size - 1) & ~(size - 1)
                    result, window, _ = owner(HOST_WINDOW, [base], outputs=3)
        if not result:
            result, _, _ = owner(INIT_DEVICE, [])
        if reservation:
            system.munmap(reservation[0], reservation[1])
        if result:
            io.IOServiceClose(session)
            io.IOServiceClose(port)
            raise DriverError(f"GPU initialization failed: {result & 0xffffffff:#x} "
                              "(scripts/read-driver-log.py shows why)")

    def call(selector, scalars, capacity):
        # The self-test runs for seconds: an async session call.
        code, values, blob = observer(selector, scalars, outputs=2, capacity=capacity)
        if code == NOT_READY:
            raise DriverError("not ready: the upstream driver is not running in an open session")
        if code == NOT_PERMITTED:
            raise DriverError("not permitted: this driver predates the CS self-test")
        if code == BUSY:
            raise DriverError("busy: another self-test is running")
        if code:
            raise DriverError(f"self-test call failed: {code:#x}")
        return values, blob

    def close():
        result = io.IOServiceClose(port)
        if init:
            io.IOServiceClose(session)
        if result:
            raise DriverError(f"observer close failed: {result & 0xffffffff:#x}")
    return call, close


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    parser.add_argument("--runs", type=int, default=1, help="repeat the test")
    parser.add_argument("--init", action="store_true",
                        help="initialize the GPU through a session client first")
    args = parser.parse_args()
    call, close = connect(args.init)
    failures = 0
    try:
        for index in range(max(args.runs, 1)):
            status, parked, result = run(call)
            failures += status != 0
            if args.json:
                print(json.dumps({"run": index, "status": status, "parked": parked, **result}))
            else:
                if args.runs > 1:
                    print(f"run {index + 1}:")
                report(status, parked, result)
    finally:
        close()
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, DriverError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
