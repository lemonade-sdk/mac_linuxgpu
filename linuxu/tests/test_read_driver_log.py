"""Cached-log reader regression; fake scalar responses only, no IOKit calls."""
import importlib.util
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "scripts" / "read-driver-log.py"
spec = importlib.util.spec_from_file_location("cached_driver_log", path)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def response(end, start, payload):
    padded = payload + bytes((-len(payload)) % 8)
    values = [end, start + len(payload), len(payload)]
    values.extend(int.from_bytes(padded[i:i + 8], "little") for i in range(0, len(padded), 8))
    return values, len(values)


class Log:
    def __init__(self, payload=b"", capacity=16384, append_on_second=b""):
        self.payload = payload
        self.capacity = capacity
        self.append_on_second = append_on_second
        self.calls = 0

    def query(self, inputs, capacity):
        assert inputs[0] == 0x4c4c4f47 and capacity == 16
        self.calls += 1
        if self.calls == 2:
            self.payload += self.append_on_second
        end = len(self.payload)
        start = max(end - self.capacity, min(inputs[1], end), 0)
        return response(end, start, self.payload[start:start + 104])


def rejected(values, count, cursor=0):
    try:
        reader.read_snapshot(lambda inputs, capacity: (values, count), cursor)
    except RuntimeError:
        return
    raise AssertionError("malformed response accepted")


initial = bytes(range(110))
appended = b"next snapshot\n" * 10
log = Log(initial, append_on_second=appended)
payload, cursor = reader.read_snapshot(log.query, 0)
assert payload == initial and cursor == len(initial)
payload, cursor = reader.read_snapshot(log.query, cursor)
assert payload == appended and cursor == len(initial) + len(appended)

warnings = []
log = Log(bytes(i % 251 for i in range(17000)))
payload, cursor = reader.read_snapshot(log.query, 7, warnings.append)
assert payload == log.payload[-16384:] and cursor == 17000
assert warnings == ["log cursor clamped: 7 -> 616"]

# A producer can overwrite the remainder of the original snapshot. Resume
# at its original end so the next read reports the loss and retains new data.
log = Log(b"a" * 110, capacity=128, append_on_second=b"b" * 300)
payload, cursor = reader.read_snapshot(log.query, 0)
assert payload == b"a" * 104 and cursor == 110
payload, cursor = reader.read_snapshot(log.query, cursor)
assert payload == b"b" * 128 and cursor == 410

for end in (0, 19):
    log = Log(b"x" * end)
    warnings = []
    assert reader.read_snapshot(log.query, (1 << 64) - 1, warnings.append) == (b"", end)
    assert warnings == [f"log cursor clamped: {(1 << 64) - 1} -> {end}"]
assert reader.read_snapshot(Log().query, 0) == (b"", 0)
assert reader.read_snapshot(Log(b"done").query, 4) == (b"", 4)

rejected([100, 100, 8], 3)                    # No payload scalar.
rejected([200, 200, 105] + [0] * 13, 16)      # Beyond ABI payload capacity.
rejected([2, 3, 1, 0], 4)                    # Next cursor beyond end.
rejected([100, 1, 2, 0], 4)                  # Byte count underflows its start.
rejected([100, 30, 1, 0], 4, cursor=40)       # Backwards, not a future clamp.
rejected([100, 30, 0], 3)                    # Empty response before end.
rejected([100, 30, 0], 17)                   # Excess output count.
rejected([100, 30, 0], 4)                    # Truncated output storage.
rejected([-1, 0, 0], 3)                     # Scalars must be unsigned.

# Session snapshot decoding and the restart/release advice.
fields, advice = reader.describe_session([1, 0, 0, 0, 0, 0, 1, 2, 0], 9)
assert fields["flags"] == [] and advice is None and fields["generation"] == 2
restart = (1 << 1) | (1 << 0) | (1 << 7) | (1 << 5) | (1 << 10)
fields, advice = reader.describe_session([1, restart, 3, (1 << 64) - 11006, 0,
                                          (1 << 64) - 19, 4, 1, 0], 9)
assert fields["quarantine_cause"] == "GPU completion uncertain (compute stop)"
assert fields["cause_code"] == -11006 and fields["isolation_result"] == -19
assert fields["release_blocker"] == "upstream driver or runtime device retained"
assert advice.startswith("restart required, do not kill the driver")
fields, advice = reader.describe_session([1, 0b1100011, 5, (1 << 64) - 16, 0, 0, 0, 1, 0], 9)
assert fields["quarantine_cause"] == "endpoint isolation failed" and fields["isolation_result"] is None
assert "MacLinuxGPUHostApp release" in advice and "do not kill" in advice
fields, advice = reader.describe_session([1, 0b11, 7, 1, 2, 0, 2, 1, 1], 9)
assert fields["observed_by"] == "DMA shutdown reservation failed"
assert "release pending (interrupt drain pending)" in advice
for values, count in (([1] * 9, 8), ([2] + [0] * 8, 9), ([1, -1] + [0] * 7, 9)):
    try:
        reader.describe_session(values, count)
    except RuntimeError:
        continue
    raise AssertionError("malformed session snapshot accepted")

# Power snapshot decoding (dext/sources/power_state.h).
fields, advice = reader.describe_power([1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0], 12)
assert fields["power_state"] == "active" and fields["power_flags"] == ["vram_preserved"]
assert advice is None
fields, advice = reader.describe_power([1, 2, 3, 0b11001, 1, 0, 1, 1, 0, 2500, 1, 0], 12)
assert fields["power_state"] == "suspended" and fields["power_cause"] == "client prepare"
assert fields["power_flags"] == ["vram_preserved", "client_hold", "kfd_quiesced"]
assert fields["low_power_holds"] == 1 and fields["last_transition_us"] == 2500
assert advice == "suspended: GPU work waits for resume (VRAM kept)"
fields, advice = reader.describe_power([1, 4, 7, 0, 8, (1 << 64) - 5, 0, 1, 1, 0, 2, 0], 12)
assert fields["power_state"] == "lost" and fields["power_cause"] == "KFD suspend failed"
assert fields["power_error"] == -5 and fields["memory_losses"] == 1
assert advice.startswith("device memory was lost")
for values, count in (([1] * 12, 11), ([2] + [0] * 11, 12), ([1, -1] + [0] * 10, 12)):
    try:
        reader.describe_power(values, count)
    except RuntimeError:
        continue
    raise AssertionError("malformed power snapshot accepted")

print("cached session snapshot: flags, cause, blocker and advice decoding passed")
print("cached power snapshot: state, flags, cause and advice decoding passed")
print("cached log reader: append/resume, ring wrap, overwrite, empty/future cursors and malformed ABI passed")
