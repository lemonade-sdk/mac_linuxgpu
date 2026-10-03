#!/usr/bin/env python3
"""Probe the GPU's own outputs and show a static test pattern on them.

The MacLinuxGPU driver runs an in-kernel DRM client (linuxu/headers/rt/
display.h) through upstream code only: connector detection and EDID modes
(drm_client_modeset_probe), a framebuffer in VRAM, the pattern, and an
atomic commit through amdgpu_dm and Display Core. Turning it off commits
the display configuration recorded before the pattern again. Display must
be enabled in the driver (scripts/activate.sh --display); otherwise every
call fails with "no display" (ENODEV).

It uses the observer client (selector 84): it never initializes the GPU or
joins a session; the GPU must already be running. With --init the script
initializes it through a session client (InitDevice) held for the whole
run; if no other client uses the GPU, the session closes when the script
exits, and a pattern still showing is turned off by that close.

  display-test.py probe                    # connectors, status, modes
  display-test.py probe --edid             # and each connected EDID, decoded
  display-test.py show                     # bars on every connected output
  display-test.py show --connector DP-2 --pattern gradient --seconds 20
  display-test.py off
  display-test.py --init show --seconds 30 # bring the GPU up, show, off

Exit status 0 when the requested operation succeeded.
"""
import argparse
import ctypes as c
import errno
import os
import struct
import sys
import time

SESSION_CLIENT, OBSERVER_CLIENT = 0, 1
RUNTIME_BUILD, INIT_DEVICE = 43, 9
SYSFS_READ = 80                # MLG_SELECTOR_SYSFS_READ
DISPLAY = 84                   # dext/sources/session_state.h MLG_SELECTOR_DISPLAY
CONFIRM = 0x44495350           # MLG_DISPLAY_CONFIRM ("DISP")
OP_PROBE, OP_SHOW, OP_OFF = 0, 1, 2
PATTERNS = {"bars": 0, "white": 1, "gradient": 2}
REPORT_MAX = 1024              # MLG_DISPLAY_REPORT_MAX
NAME_MAX = 31                  # MLG_DISPLAY_NAME_MAX
SYSFS_CHUNK = 4096
NOT_READY = 0xe00002d8
NOT_PERMITTED = 0xe00002e2
BUSY = 0xe00002d5
BAD_ARGUMENT = 0xe00002c2

# struct rt_display_report (linuxu/headers/rt/display.h), version 1.
VERSION = 1
HEADER = struct.Struct("<8I3Q3iI")
CONNECTOR = struct.Struct("<32s12I")
CONNECTORS_MAX = 8
REPORT_SIZE = HEADER.size + CONNECTORS_MAX * CONNECTOR.size
STATUS = {1: "connected", 2: "disconnected", 3: "unknown"}


class DriverError(RuntimeError):
    pass


def signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def errno_text(status):
    if not status:
        return "ok"
    code = -status
    name = errno.errorcode.get(code, "error")
    meaning = {errno.ENODEV: "no display: the driver runs without Display Core (activate.sh --display)",
               errno.ENOENT: "no connected output (or no connector by that name)",
               errno.E2BIG: "the outputs' modes need a framebuffer larger than 8192 pixels"}.get(code)
    return f"Linux errno {code} ({name}{': ' + meaning if meaning else ''})"


def decode(blob):
    """struct rt_display_report as a dict."""
    if len(blob) < REPORT_SIZE:
        raise DriverError(f"display report is {len(blob)} bytes, expected {REPORT_SIZE}")
    head = HEADER.unpack_from(blob)
    if head[0] != VERSION:
        raise DriverError(f"display report version {head[0]} is not this reader's")
    keys = ["version", "connectors", "showing", "pattern", "fb_width", "fb_height", "fb_pitch",
            "crtcs", "fb_gpu_addr", "fill_ns", "commit_ns", "probe_status", "commit_status",
            "restore_status", "reserved"]
    report = dict(zip(keys, head))
    if report["connectors"] > CONNECTORS_MAX:
        raise DriverError("display report lists too many connectors")
    report["connector"] = []
    for i in range(report["connectors"]):
        fields = CONNECTOR.unpack_from(blob, HEADER.size + i * CONNECTOR.size)
        name = fields[0].split(b"\0", 1)[0].decode(errors="replace")
        values = dict(zip(["id", "status", "modes", "edid_bytes", "preferred_width",
                           "preferred_height", "preferred_refresh", "lit", "lit_width",
                           "lit_height", "lit_refresh", "crtc"], fields[1:]))
        values["name"] = name
        report["connector"].append(values)
    return report


def display_op(call, op, pattern=0, connector=None):
    """One Display call: (status, report)."""
    name = connector.encode() if connector else b""
    if len(name) > NAME_MAX:
        raise DriverError(f"connector name is longer than {NAME_MAX} bytes")
    values, blob = call(DISPLAY, [op, pattern, CONFIRM], name, REPORT_MAX)
    if not values:
        raise DriverError("display reply without status")
    return signed(values[0]), decode(blob)


def report_text(report, out=sys.stdout):
    for c in report["connector"]:
        line = f"  {c['name']:<10} {STATUS.get(c['status'], c['status'])}"
        if c["status"] == 1:
            line += (f", {c['modes']} mode(s), preferred {c['preferred_width']}x{c['preferred_height']}"
                     f"@{c['preferred_refresh']}, EDID {c['edid_bytes']} bytes")
        if c["lit"]:
            line += f"; showing {c['lit_width']}x{c['lit_height']}@{c['lit_refresh']} on CRTC {c['crtc']}"
        print(line, file=out)
    print(f"  {report['crtcs']} CRTC(s)", file=out)
    if report["showing"]:
        name = {v: k for k, v in PATTERNS.items()}.get(report["pattern"], report["pattern"])
        print(f"  pattern {name}: framebuffer {report['fb_width']}x{report['fb_height']} "
              f"(pitch {report['fb_pitch']}) at VRAM {report['fb_gpu_addr']:#x}, "
              f"written in {report['fill_ns'] / 1e6:.1f} ms, committed in {report['commit_ns'] / 1e6:.1f} ms",
              file=out)


# ---- EDID ----

def decode_edid(data):
    """The EDID's identity, size and preferred timing (EDID 1.3/1.4 base block)."""
    if len(data) < 128 or data[:8] != bytes([0, 255, 255, 255, 255, 255, 255, 0]):
        raise ValueError("not an EDID base block")
    if sum(data[:128]) & 0xff:
        raise ValueError("EDID base block checksum mismatch")
    word = data[8] << 8 | data[9]
    vendor = "".join(chr(((word >> s) & 31) + 64) for s in (10, 5, 0))
    edid = {"vendor": vendor, "product": data[10] | data[11] << 8,
            "serial": struct.unpack_from("<I", data, 12)[0],
            "week": data[16], "year": data[17] + 1990, "version": f"{data[18]}.{data[19]}",
            "digital": bool(data[20] & 0x80), "width_cm": data[21], "height_cm": data[22],
            "extensions": data[126], "name": None, "serial_text": None, "preferred": None}
    for at in range(54, 126, 18):
        d = data[at:at + 18]
        if d[0] or d[1]:
            if edid["preferred"] is None:
                clock = (d[0] | d[1] << 8) * 10_000
                h = d[2] | (d[4] & 0xf0) << 4
                hblank = d[3] | (d[4] & 0x0f) << 8
                v = d[5] | (d[7] & 0xf0) << 4
                vblank = d[6] | (d[7] & 0x0f) << 8
                width_mm = d[12] | (d[14] & 0xf0) << 4
                height_mm = d[13] | (d[14] & 0x0f) << 8
                total = (h + hblank) * (v + vblank)
                edid["preferred"] = {"width": h, "height": v, "clock_hz": clock,
                                     "refresh": clock / total if total else 0,
                                     "width_mm": width_mm, "height_mm": height_mm}
            continue
        text = d[5:18].split(b"\n", 1)[0].decode("ascii", errors="replace").strip()
        if d[3] == 0xfc:
            edid["name"] = text
        elif d[3] == 0xff:
            edid["serial_text"] = text
    return edid


def edid_text(edid):
    p = edid["preferred"]
    parts = [f"{edid['vendor']} product {edid['product']:#06x}",
             f"name {edid['name']!r}" if edid["name"] else "no name",
             f"serial {edid['serial_text'] or edid['serial']}",
             f"made {edid['year']} week {edid['week']}", f"EDID {edid['version']}",
             f"{edid['width_cm']}x{edid['height_cm']} cm", f"{edid['extensions']} extension(s)"]
    if p:
        parts.append(f"preferred {p['width']}x{p['height']}@{p['refresh']:.2f} "
                     f"({p['clock_hz'] / 1e6:.2f} MHz, {p['width_mm']}x{p['height_mm']} mm)")
    return ", ".join(parts)


def sysfs_read(call, path, op=0):
    data = bytearray()
    while True:
        values, payload = call(SYSFS_READ, [op, len(data)], path.encode(), SYSFS_CHUNK)
        status, count, length = values[0], values[1], values[2]
        status = signed(status)
        if status:
            raise OSError(-status, f"{path}: {errno_text(status)}")
        data.extend(payload[:count])
        # A bin file of unknown size (length 0, the EDID) ends at a short read.
        if not count or (length and len(data) >= length) or (not length and count < SYSFS_CHUNK):
            return bytes(data)


def connector_edid(call, name):
    """The connector's EDID file, under the DRM card directory the device
    holds, as Linux shows /sys/class/drm/card0-DP-1/edid."""
    listing = sysfs_read(call, "drm", op=1).decode().split("\n")
    cards = sorted(line[2:] for line in listing if line.startswith("d card") and "-" not in line)
    for card in cards:
        try:
            return sysfs_read(call, f"drm/{card}/{card}-{name}/edid")
        except OSError as error:
            if error.errno != errno.ENOENT:
                raise
    raise OSError(errno.ENOENT, f"no sysfs connector directory for {name}")


# ---- IOKit ----

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
    if init:
        build = (c.c_uint64 * 3)()
        count = c.c_uint(3)
        result = io.IOConnectCallScalarMethod(session, RUNTIME_BUILD, None, 0, build, c.byref(count))
        if not result:
            count = c.c_uint(0)
            result = io.IOConnectCallScalarMethod(session, INIT_DEVICE, None, 0, None, c.byref(count))
        if result:
            io.IOServiceClose(session)
            io.IOServiceClose(port)
            raise DriverError(f"GPU initialization failed: {result & 0xffffffff:#x} "
                              "(scripts/read-driver-log.py shows why)")

    def call(selector, scalars, data, capacity):
        inputs = (c.c_uint64 * len(scalars))(*scalars)
        outputs = (c.c_uint64 * 3)()
        count = c.c_uint(3)
        out = c.create_string_buffer(capacity)
        size = c.c_size_t(capacity)
        source = c.create_string_buffer(data, len(data)) if data else None
        result = io.IOConnectCallMethod(port, selector, inputs, len(scalars),
                                        source, len(data) if data else 0,
                                        outputs, c.byref(count), out, c.byref(size))
        code = result & 0xffffffff
        if code == NOT_READY:
            raise DriverError("not ready: the upstream driver is not running in an open session "
                              "(start it with --init, or keep a session client open)")
        if code == NOT_PERMITTED:
            raise DriverError("not permitted: this driver predates the display test (selector 84)")
        if code == BUSY:
            raise DriverError("busy: another display operation is running")
        if code == BAD_ARGUMENT:
            raise DriverError("bad argument (connector name or pattern)")
        if result:
            raise DriverError(f"call {selector} failed: {code:#x}")
        return list(outputs[:count.value]), out.raw[:size.value]

    def close():
        result = io.IOServiceClose(port)
        if init:
            io.IOServiceClose(session)
        if result:
            raise DriverError(f"observer close failed: {result & 0xffffffff:#x}")
    return call, close


def run(call, args, out=sys.stdout, sleep=time.sleep):
    """The requested operation; returns the exit status."""
    if args.command == "probe":
        status, report = display_op(call, OP_PROBE)
        print(f"probe: {errno_text(status)}", file=out)
        report_text(report, out)
        if args.edid:
            for conn in report["connector"]:
                if conn["status"] != 1:
                    continue
                try:
                    data = connector_edid(call, conn["name"])
                    print(f"  {conn['name']} EDID ({len(data)} bytes): {edid_text(decode_edid(data))}",
                          file=out)
                    for at in range(0, len(data), 16):
                        print(f"    {at:04x}  {data[at:at + 16].hex(' ')}", file=out)
                except (OSError, ValueError) as error:
                    print(f"  {conn['name']} EDID: {error}", file=out)
                    status = status or -errno.EIO
        return 0 if status == 0 else 1
    if args.command == "off":
        status, report = display_op(call, OP_OFF)
        print(f"off: {errno_text(status)}", file=out)
        report_text(report, out)
        return 0 if status == 0 else 1
    status, report = display_op(call, OP_SHOW, PATTERNS[args.pattern], args.connector)
    print(f"show {args.pattern} on {args.connector or 'every connected output'}: {errno_text(status)}",
          file=out)
    if status:
        print(f"  probe {errno_text(report['probe_status'])}, commit {errno_text(report['commit_status'])}, "
              f"restore {errno_text(report['restore_status'])}", file=out)
    report_text(report, out)
    if status:
        return 1
    if not args.seconds:
        return 0
    try:
        sleep(args.seconds)
    finally:
        # Also on Ctrl-C: the pattern does not outlive the requested time.
        off_status, off_report = display_op(call, OP_OFF)
        print(f"off after {args.seconds} s: {errno_text(off_status)}", file=out)
        if off_status:
            print(f"  restore {errno_text(off_report['restore_status'])}", file=out)
    return 0 if off_status == 0 else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--init", action="store_true",
                        help="initialize the GPU through a session client held for the run")
    sub = parser.add_subparsers(dest="command", required=True)
    probe = sub.add_parser("probe", help="detect connectors and list their modes")
    probe.add_argument("--edid", action="store_true", help="read and decode each connected EDID")
    show = sub.add_parser("show", help="show a test pattern")
    show.add_argument("--connector", help="one connector (e.g. DP-1); default every connected output")
    show.add_argument("--pattern", choices=sorted(PATTERNS), default="bars")
    show.add_argument("--seconds", type=float, default=0,
                      help="turn the pattern off after this many seconds (default: leave it on)")
    sub.add_parser("off", help="turn the test pattern off, restoring the previous configuration")
    args = parser.parse_args()
    if args.init and args.command == "show" and not args.seconds:
        # The session (and with it the pattern) ends when this script exits.
        args.seconds = 30
    call, close = connect(args.init)
    try:
        return run(call, args)
    finally:
        close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, DriverError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
