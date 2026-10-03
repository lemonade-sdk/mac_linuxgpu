#!/usr/bin/env python3
"""Read the amdgpu device's sysfs files through the MacLinuxGPU observer client.

Paths are relative to the device directory a Linux tool reads under
/sys/class/drm/card0/device, for example:

  read-sysfs.py gpu_busy_percent mem_info_vram_used pp_dpm_sclk
  read-sysfs.py --list hwmon/hwmon0
  read-sysfs.py --hex gpu_metrics
  read-sysfs.py --sensor gpu_load --grbm 100

Each file read runs the attribute's show() (or a bin attribute's read())
in the driver, exactly as a Linux sysfs read does. The observer client never
claims PCI, joins the session or touches queues; reads fail with "not ready"
unless the upstream driver is running.
"""
import argparse
import ctypes as c
import errno
import os
import struct
import sys
import time

OBSERVER_CLIENT = 1
SYSFS_READ = 80          # dext/sources/session_state.h MLG_SELECTOR_SYSFS_READ
DRM_INFO = 81            # MLG_SELECTOR_DRM_INFO
OP_READ, OP_LIST = 0, 1
CHUNK = 4096             # MLG_SYSFS_CHUNK_MAX
PATH_MAX = 256           # MLG_SYSFS_PATH_MAX
NOT_READY = 0xe00002d8
NOT_PERMITTED = 0xe00002e2

# include/uapi/drm/amdgpu_drm.h
INFO_SENSOR = 0x1D
INFO_READ_MMR_REG = 0x15
SENSORS = {"gfx_sclk": 1, "gfx_mclk": 2, "gpu_temp": 3, "gpu_load": 4, "gpu_avg_power": 5,
           "vddnb": 6, "vddgfx": 7, "gpu_input_power": 0xc}
# SOC15 GC register layout (gc_*_offset.h regGRBM_STATUS, base index 0) and
# GRBM_STATUS__GUI_ACTIVE (gc_*_sh_mask.h); the segment base comes from the
# device's IP discovery table in sysfs.
GRBM_STATUS = 0x0da4
GUI_ACTIVE = 1 << 31


class DriverError(RuntimeError):
    pass


def signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def check_errno(status, what):
    status = signed(status)
    if status:
        code = -status
        raise OSError(code, f"{what}: {os.strerror(code) if 0 < code < 200 else 'error'} (Linux errno {code})")


def read_file(call, path, op=OP_READ):
    """The whole file (or listing): read in chunks until its full length.
    A listing of "" is the device directory itself."""
    if (not path and op != OP_LIST) or len(path.encode()) > PATH_MAX:
        raise ValueError(f"path must be 1 to {PATH_MAX} bytes")
    data = bytearray()
    while True:
        values, payload = call(SYSFS_READ, [op, len(data)], path.encode(), CHUNK)
        status, count, length = values[:3]
        check_errno(status, path)
        if count != len(payload) or count > CHUNK:
            raise DriverError(f"{path}: inconsistent reply")
        data.extend(payload)
        if not count or len(data) >= length:
            return bytes(data[:length]) if length else bytes(data)


def list_dir(call, path):
    """[(type, name)] with type 'f' file, 'd' directory or 'l' link."""
    entries = []
    for line in read_file(call, path, OP_LIST).decode().splitlines():
        kind, _, name = line.partition(" ")
        if kind not in ("f", "d", "l") or not name:
            raise DriverError(f"{path}: bad listing line {line!r}")
        entries.append((kind, name))
    return entries


def metrics_header(blob):
    """struct metrics_table_header: (structure_size, format, content)."""
    if len(blob) < 4:
        raise DriverError("gpu_metrics shorter than its header")
    size, frev, crev = struct.unpack_from("<HBB", blob)
    return size, frev, crev


def drm_info(call, query, args, size):
    values, payload = call(DRM_INFO, [query, size], args, size)
    check_errno(values[0], f"AMDGPU_INFO {query:#x}")
    if len(payload) != size:
        raise DriverError("short AMDGPU_INFO reply")
    return payload


def gc_base(call):
    """GC segment 0 base from ip_discovery/die/0/GC/0/base_addr."""
    text = read_file(call, "ip_discovery/die/0/GC/0/base_addr").decode()
    return int(text.split()[0], 16)


def grbm_busy(call, samples, interval):
    """Fraction of GRBM_STATUS samples with GUI_ACTIVE set, as amdgpu_top."""
    offset = gc_base(call) + GRBM_STATUS
    args = struct.pack("<IIII", offset, 1, 0xffffffff, 0)
    active = 0
    for _ in range(samples):
        value, = struct.unpack("<I", drm_info(call, INFO_READ_MMR_REG, args, 4))
        active += bool(value & GUI_ACTIVE)
        time.sleep(interval)
    return active / samples


def connect():
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
    service = io.IOServiceGetMatchingService(0, io.IOServiceNameMatching(b"MacLinuxGPU"))
    if not service:
        raise DriverError("MacLinuxGPU registry service was not found")
    port = c.c_uint()
    try:
        task = c.c_uint.in_dll(system, "mach_task_self_").value
        result = io.IOServiceOpen(service, task, OBSERVER_CLIENT, c.byref(port))
    finally:
        io.IOObjectRelease(service)
    if result:
        raise DriverError(f"observer open failed: {result & 0xffffffff:#x}")

    def call(selector, scalars, data, capacity):
        inputs = (c.c_uint64 * len(scalars))(*scalars)
        outputs = (c.c_uint64 * 3)()
        count = c.c_uint(3)
        out = c.create_string_buffer(max(capacity, 1))
        size = c.c_size_t(capacity)
        source = c.create_string_buffer(data, len(data)) if data else None
        result = io.IOConnectCallMethod(port, selector, inputs, len(scalars),
                                        source, len(data) if data else 0,
                                        outputs, c.byref(count), out, c.byref(size))
        code = result & 0xffffffff
        if code == NOT_READY:
            raise DriverError("not ready: the upstream driver is not running in an open session")
        if code == NOT_PERMITTED:
            raise DriverError("selector not permitted for observers (driver predates SysfsRead?)")
        if result:
            raise DriverError(f"call {selector} failed: {code:#x}")
        return list(outputs[:count.value]), out.raw[:size.value]

    def close():
        result = io.IOServiceClose(port)
        if result:
            raise DriverError(f"observer close failed: {result & 0xffffffff:#x}")
    return call, close


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="files (or, with --list, directories; '' is the device directory)")
    parser.add_argument("--list", action="store_true", help="list directories instead")
    parser.add_argument("--hex", action="store_true", help="hex dump instead of text")
    parser.add_argument("--sensor", action="append", choices=sorted(SENSORS),
                        help="AMDGPU_INFO_SENSOR query (repeatable)")
    parser.add_argument("--grbm", type=int, metavar="N",
                        help="sample GRBM_STATUS N times at 10 ms and print GUI_ACTIVE %%")
    args = parser.parse_args()
    if not args.paths and not args.sensor and not args.grbm:
        parser.error("nothing to read")
    call, close = connect()
    failed = False
    try:
        for path in args.paths:
            try:
                if args.list:
                    for kind, name in list_dir(call, path):
                        print(f"{kind} {path.rstrip('/') + '/' if path else ''}{name}")
                    continue
                data = read_file(call, path)
                if args.hex or path.endswith("gpu_metrics"):
                    if path.endswith("gpu_metrics") and len(data) >= 4:
                        size, frev, crev = metrics_header(data)
                        print(f"{path}: gpu_metrics v{frev}.{crev}, {size} bytes")
                    for at in range(0, len(data), 16):
                        print(f"{at:04x}  {data[at:at + 16].hex(' ')}")
                else:
                    text = data.decode(errors="replace")
                    print(f"{path}: {text}" if "\n" not in text.rstrip("\n") else f"{path}:\n{text}",
                          end="" if text.endswith("\n") else "\n")
            except (OSError, ValueError, DriverError) as error:
                print(f"{path}: {error}", file=sys.stderr)
                failed = True
        for name in args.sensor or []:
            try:
                value, = struct.unpack("<I", drm_info(call, INFO_SENSOR, struct.pack("<I", SENSORS[name]), 4))
                print(f"sensor {name}: {value}")
            except (OSError, DriverError) as error:
                print(f"sensor {name}: {error}", file=sys.stderr)
                failed = True
        if args.grbm:
            try:
                print(f"GRBM_STATUS GUI_ACTIVE: {100 * grbm_busy(call, args.grbm, 0.01):.1f}% "
                      f"of {args.grbm} samples")
            except (OSError, ValueError, DriverError) as error:
                print(f"GRBM_STATUS: {error}", file=sys.stderr)
                failed = True
    finally:
        close()
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, DriverError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
