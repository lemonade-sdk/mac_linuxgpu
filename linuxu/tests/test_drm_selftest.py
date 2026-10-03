"""drm-selftest.py against a fake DrmSelfTest selector; no IOKit calls."""
import importlib.util
import io
import struct
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "scripts" / "drm-selftest.py"
spec = importlib.util.spec_from_file_location("drm_selftest", path)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

# struct rt_cs_selftest_result (linuxu/headers/rt/cs_selftest.h) is 200 bytes.
assert tool.LAYOUT.size == 200 and len(tool.STEPS) == 18 and len(tool.FIELDS) == 16


def blob(status, failed, passed, version=1, steps=18):
    statuses = list(status) + [0] * (24 - len(status))
    return tool.LAYOUT.pack(version, steps, passed, failed, *statuses,
                            152, 0x40, 0x7551, 4, 1, 3,
                            0x400000, 0xc00000, 7, 9, 1_500_000, 2_000_000,
                            0xc0de0001, 0xc0de0002, 0x5eed5eed, 7)


class Driver:
    def __init__(self, reply, status=0, parked=0):
        self.reply, self.status, self.parked, self.calls = reply, status, parked, []

    def call(self, selector, scalars, capacity):
        self.calls.append((selector, scalars, capacity))
        return [self.status & (2 ** 64 - 1), self.parked], self.reply


# A passing run.
ok = Driver(blob([0] * 18, 18, (1 << 18) - 1))
status, parked, result = tool.run(ok.call)
assert ok.calls == [(82, [0x43535354], 512)]
assert status == 0 and parked == 0 and result["failed_step"] is None
assert result["steps"]["compute_cs"] == 0 and result["device_id"] == 0x7551
assert result["compute_value"] == 0xc0de0001 and result["sdma_seq"] == 9
text = io.StringIO()
tool.report(status, parked, result, text)
assert "PASS" in text.getvalue() and "compute_wait_cs  passed" in text.getvalue()
assert "1.500 ms" in text.getvalue()

# A hung compute queue: the wait timed out (-ETIME), the rest did not run,
# the process is parked.
steps = [0] * 10 + [-62] + [1] * 6 + [4]
hung = Driver(blob(steps, 10, (1 << 10) - 1), status=-62, parked=1)
status, parked, result = tool.run(hung.call)
assert status == -62 and parked == 1 and result["failed_step"] == "compute_wait_cs"
text = io.StringIO()
tool.report(status, parked, result, text)
out = text.getvalue()
assert "FAIL at compute_wait_cs: failed: Linux errno 62" in out
assert "not run" in out and "parked" in out and "1 self-test process(es)" in out

# Results this reader does not understand are refused.
for bad in (blob([0], 18, 0, version=2), blob([0], 18, 0, steps=17), b"\0" * 64):
    try:
        tool.run(Driver(bad).call)
    except tool.DriverError:
        pass
    else:
        raise AssertionError("malformed result accepted")
assert tool.describe(3).startswith("mismatch") and tool.describe(2).startswith("skipped")
print("PASS drm-selftest.py: request, result layout, step reports, parked and failed runs")
