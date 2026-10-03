"""display-test.py against a fake Display/SysfsRead selector; no IOKit calls."""
import argparse
import errno
import importlib.util
import io
import struct
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "scripts" / "display-test.py"
spec = importlib.util.spec_from_file_location("display_test", path)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

# struct rt_display_report (linuxu/headers/rt/display.h): 72 + 8 * 80 bytes.
assert tool.HEADER.size == 72 and tool.CONNECTOR.size == 80 and tool.REPORT_SIZE == 712
assert tool.DISPLAY == 84 and tool.CONFIRM == 0x44495350


def edid_block():
    """A base block: "LNX" 0x0001, 60x34 cm, 1920x1080@60 preferred, a name."""
    b = bytearray(128)
    b[0:8] = bytes([0, 255, 255, 255, 255, 255, 255, 0])
    b[8:10] = bytes([0x31, 0xd8])
    b[10] = 1
    b[12:16] = struct.pack("<I", 4242)
    b[16], b[17], b[18], b[19], b[20], b[21], b[22] = 1, 36, 1, 4, 0xa5, 60, 34
    b[54:72] = bytes([0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00,
                      0x58, 0x54, 0x21, 0x00, 0x00, 0x1e])
    b[72:90] = b"\0\0\0\xfc\0TEST PANEL\n  "
    b[127] = (-sum(b[:127])) & 0xff
    return bytes(b)


def report(connectors, showing=0, pattern=0, fb=(0, 0, 0), commit=(0, 0, 0)):
    blob = tool.HEADER.pack(1, len(connectors), showing, pattern, fb[0], fb[1], fb[2], 4,
                            0x8003000000 if showing else 0, 5_000_000, 40_000_000,
                            commit[0], commit[1], commit[2], 0)
    for c in connectors:
        blob += tool.CONNECTOR.pack(c[0].encode(), *c[1:])
    return blob + bytes(tool.REPORT_SIZE - len(blob))


DP1 = ("DP-1", 90, 1, 12, 128, 1920, 1080, 60, 0, 0, 0, 0, 0)
DP1_LIT = ("DP-1", 90, 1, 12, 128, 1920, 1080, 60, 1, 1920, 1080, 60, 0)
DP2 = ("DP-2", 95, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)


class Driver:
    def __init__(self, replies):
        self.replies, self.calls = list(replies), []

    def call(self, selector, scalars, data, capacity):
        self.calls.append((selector, list(scalars), data))
        if selector == tool.SYSFS_READ:
            op, offset = scalars
            name = data.decode()
            if op == 1 and name == "drm":
                content = b"d card0\n"
            elif name == "drm/card0/card0-DP-1/edid":
                content = edid_block()
            else:
                return [(-errno.ENOENT) & (2 ** 64 - 1), 0, 0], b""
            chunk = content[offset:offset + tool.SYSFS_CHUNK]
            return [0, len(chunk), 0 if name.endswith("edid") else len(content)], chunk
        assert selector == tool.DISPLAY and capacity == tool.REPORT_MAX
        status, blob = self.replies.pop(0)
        return [status & (2 ** 64 - 1)], blob


def args(command, **kw):
    values = {"command": command, "edid": False, "connector": None, "pattern": "bars", "seconds": 0}
    values.update(kw)
    return argparse.Namespace(**values)


# Probe with the EDID decoded.
d = Driver([(0, report([DP1, DP2]))])
out = io.StringIO()
assert tool.run(d.call, args("probe", edid=True), out) == 0
assert d.calls[0] == (84, [0, 0, 0x44495350], b"")
text = out.getvalue()
assert "DP-1       connected, 12 mode(s), preferred 1920x1080@60, EDID 128 bytes" in text
assert "DP-2       disconnected" in text
assert "LNX product 0x0001, name 'TEST PANEL', serial 4242" in text
assert "preferred 1920x1080@60.00 (148.50 MHz, 600x340 mm)" in text
assert "60x34 cm" in text

# Show on one connector for a while, then off; the request carries the name.
slept = []
d = Driver([(0, report([DP1_LIT, DP2], showing=1, pattern=2, fb=(1920, 1080, 7680))),
            (0, report([DP1, DP2]))])
out = io.StringIO()
assert tool.run(d.call, args("show", connector="DP-1", pattern="gradient", seconds=3), out,
                sleep=slept.append) == 0
assert slept == [3] and d.calls[0] == (84, [1, 2, 0x44495350], b"DP-1")
assert d.calls[1] == (84, [2, 0, 0x44495350], b"")
text = out.getvalue()
assert "show gradient on DP-1: ok" in text and "showing 1920x1080@60 on CRTC 0" in text
assert "framebuffer 1920x1080 (pitch 7680)" in text and "off after 3 s: ok" in text

# A failed commit is reported with each step's errno, and nothing else runs.
d = Driver([(-errno.EINVAL, report([DP1, DP2], commit=(0, -errno.EINVAL, 0)))])
out = io.StringIO()
assert tool.run(d.call, args("show", seconds=5), out, sleep=lambda s: None) == 1
assert len(d.calls) == 1
assert "Linux errno 22 (EINVAL)" in out.getvalue() and "restore ok" in out.getvalue()

# No display, no connected output.
d = Driver([(-errno.ENODEV, report([]))])
out = io.StringIO()
assert tool.run(d.call, args("probe"), out) == 1 and "no display" in out.getvalue()
d = Driver([(-errno.ENOENT, report([DP2]))])
out = io.StringIO()
assert tool.run(d.call, args("show"), out) == 1
assert "every connected output: Linux errno 2 (ENOENT: no connected output" in out.getvalue()

# Off reports the restore.
d = Driver([(0, report([DP1]))])
out = io.StringIO()
assert tool.run(d.call, args("off"), out) == 0 and d.calls[0][1] == [2, 0, 0x44495350]

# Reports this reader does not understand, and over-long names, are refused.
for bad in (b"\0" * 100, report([]).replace(b"\x01", b"\x02", 1)):
    try:
        tool.decode(bad)
    except tool.DriverError:
        pass
    else:
        raise AssertionError("malformed report accepted")
try:
    tool.display_op(Driver([]).call, 1, 0, "X" * 40)
except tool.DriverError:
    pass
else:
    raise AssertionError("long connector name accepted")
for bad in (b"\0" * 128, edid_block()[:-1] + b"\0"):
    try:
        tool.decode_edid(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("bad EDID accepted")
print("PASS display-test.py: probe/show/off requests, report layout, EDID decode, errors")
