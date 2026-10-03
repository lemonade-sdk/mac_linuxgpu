"""read-sysfs.py regression against a fake SysfsRead/DrmInfo; no IOKit calls."""
import errno
import importlib.util
import struct
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "scripts" / "read-sysfs.py"
spec = importlib.util.spec_from_file_location("read_sysfs", path)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def failure(code, words=3):
    return [(-code) & (2 ** 64 - 1)] + [0] * (words - 1)


class Driver:
    """The selector ABI of dext/sources/session_state.h over a dict tree."""

    def __init__(self, files, dirs):
        self.files, self.dirs, self.calls = files, dirs, 0
        self.grbm = [0x80000000, 0, 0x80000000, 0x80000000]

    def call(self, selector, scalars, data, capacity):
        self.calls += 1
        if selector == reader.DRM_INFO:
            query, size = scalars
            if query == reader.INFO_SENSOR:
                assert struct.unpack("<I", data) == (4,) and size == 4
                return [0], struct.pack("<I", 37)
            assert query == reader.INFO_READ_MMR_REG and size == 4
            offset, count, instance, flags = struct.unpack("<IIII", data)
            if offset != 0x1260 + 0x0da4:
                return failure(errno.EFAULT, 1), b""
            assert count == 1 and instance == 0xffffffff and not flags
            return [0], struct.pack("<I", self.grbm[self.calls % len(self.grbm)])
        assert selector == reader.SYSFS_READ and len(scalars) == 2
        op, offset = scalars
        name = data.decode()
        if op == reader.OP_LIST:
            if name not in self.dirs:
                return failure(errno.ENOTDIR if name in self.files else errno.ENOENT), b""
            content = "".join(f"{kind} {entry}\n" for kind, entry in self.dirs[name]).encode()
        else:
            if name in self.dirs:
                return failure(errno.EISDIR), b""
            if name not in self.files:
                return failure(errno.ENOENT), b""
            content = self.files[name]
        chunk = content[offset:offset + min(capacity, reader.CHUNK)]
        # A bin attribute of size 0 (drm_sysfs's edid) reports no length.
        return [0, len(chunk), 0 if name.endswith("/edid") else len(content)], chunk


def expect_errno(code, function, *args):
    try:
        function(*args)
    except OSError as error:
        assert error.errno == code, (error.errno, code)
    else:
        raise AssertionError(f"expected errno {code}")


metrics = struct.pack("<HBB", 120, 1, 3) + bytes(116)
long_listing = [("f", f"attribute_{i:04d}") for i in range(400)]
driver = Driver({"gpu_busy_percent": b"37\n", "gpu_metrics": metrics,
                 "pp_dpm_sclk": b"0: 500Mhz \n1: 2450Mhz *\n", "big": bytes(range(256)) * 40,
                 "ip_discovery/die/0/GC/0/base_addr": b"0x00001260\n0x0000A000\n",
                 "drm/card0/card0-DP-1/edid": bytes(range(256)),
                 "drm/card0/card0-DP-2/edid": bytes(range(256)) * 32},
                {"hwmon": [("d", "hwmon3")], "many": long_listing,
                 "": [("f", "gpu_busy_percent"), ("d", "hwmon")]})
assert reader.read_file(driver.call, "gpu_busy_percent") == b"37\n"
assert reader.read_file(driver.call, "big") == bytes(range(256)) * 40     # three chunks
# Unknown-length bin files end at a short read: one EDID, and one of two
# full chunks.
assert reader.read_file(driver.call, "drm/card0/card0-DP-1/edid") == bytes(range(256))
assert reader.read_file(driver.call, "drm/card0/card0-DP-2/edid") == bytes(range(256)) * 32
assert reader.metrics_header(reader.read_file(driver.call, "gpu_metrics")) == (120, 1, 3)
assert reader.list_dir(driver.call, "hwmon") == [("d", "hwmon3")]
assert reader.list_dir(driver.call, "many") == long_listing                 # paged listing
assert reader.list_dir(driver.call, "") == [("f", "gpu_busy_percent"), ("d", "hwmon")]  # the device directory
expect_errno(errno.ENOENT, reader.read_file, driver.call, "missing")
expect_errno(errno.EISDIR, reader.read_file, driver.call, "hwmon")
expect_errno(errno.ENOTDIR, reader.list_dir, driver.call, "gpu_busy_percent")
for bad in ("", "x" * (reader.PATH_MAX + 1)):
    try:
        reader.read_file(driver.call, bad)
    except ValueError:
        pass
    else:
        raise AssertionError("path bound")
assert reader.drm_info(driver.call, reader.INFO_SENSOR, struct.pack("<I", 4), 4) == struct.pack("<I", 37)
assert reader.gc_base(driver.call) == 0x1260
assert reader.grbm_busy(driver.call, 8, 0) == 0.75
driver.files["ip_discovery/die/0/GC/0/base_addr"] = b"0x00002000\n"
expect_errno(errno.EFAULT, reader.grbm_busy, driver.call, 1, 0)
print("PASS read-sysfs: chunked reads, paged listings, errnos, path bounds, gpu_metrics header, AMDGPU_INFO, GRBM sampling")
