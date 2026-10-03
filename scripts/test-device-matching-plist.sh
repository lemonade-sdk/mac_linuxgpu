#!/usr/bin/env bash
# Assert that the dext personality, entitlements and host plist name no
# device: matching is by AMD vendor ID plus the PCI classes upstream amdgpu
# binds, evaluated with IOPCIFamily's "value&mask value&mask" semantics.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
import plistlib
import re
import sys

def load(path):
    with open(path, "rb") as f:
        return plistlib.load(f)

def entries(spec, default_mask):
    """IOPCIBridge::matchKeys: space-separated value[&mask] alternatives."""
    out = []
    for token in spec.split():
        value, _, mask = token.partition("&")
        out.append((int(value, 16), int(mask, 16) if mask else default_mask))
    assert out, spec
    return out

def matches(spec, register, default_mask):
    return any((v & m) == (register & m) for v, m in entries(spec, default_mask))

failures = []
def check(cond, message):
    if not cond:
        failures.append(message)

info = load("dext/Info.plist")
personalities = info["IOKitPersonalities"]
check(personalities, "no IOKit personality")
for name, p in personalities.items():
    check(p.get("IOProviderClass") == "IOPCIDevice", f"{name}: not an IOPCIDevice personality")
    check(p.get("IOPCITunnelCompatible") is True, f"{name}: Thunderbolt-tunneled devices not matched")
    for key in ("IOPCIMatch", "IOPCISecondaryMatch", "IONameMatch", "IOPropertyMatch"):
        check(key not in p, f"{name}: {key} would bind specific devices")
    primary = p.get("IOPCIPrimaryMatch")
    check(primary is not None, f"{name}: missing vendor match")
    if primary:
        for value, mask in entries(primary, 0xFFFFFFFF):
            check(mask & 0xFFFF0000 == 0, f"{name}: IOPCIPrimaryMatch {primary} constrains the device ID")
            check(value & mask == 0x1002, f"{name}: IOPCIPrimaryMatch {primary} is not the AMD vendor")
        # Several devices from different families, and a non-AMD vendor.
        for device in (0x6798, 0x67DF, 0x687F, 0x731F, 0x73BF, 0x744C, 0x7550, 0x15BF):
            check(matches(primary, (device << 16) | 0x1002, 0xFFFFFFFF),
                  f"{name}: AMD device {device:04x} not matched by vendor")
        check(not matches(primary, (0x2684 << 16) | 0x10DE, 0xFFFFFFFF),
              f"{name}: non-AMD vendor matched")
    klass = p.get("IOPCIClassMatch")
    check(klass is not None, f"{name}: no class filter; non-GPU AMD functions would be claimed")
    if klass:
        def cls(code24, rev=0):
            return (code24 << 8) | rev
        for code, what in ((0x030000, "VGA"), (0x038000, "display other"),
                           (0x030200, "3D controller"), (0x120000, "processing accelerator")):
            check(matches(klass, cls(code, 0xC1), 0xFFFFFF00), f"{name}: {what} class not matched")
        for code, what in ((0x040300, "HDMI/DP audio"), (0x0C0330, "USB xHCI"),
                           (0x0C8000, "serial bus other"), (0x060400, "PCI bridge"),
                           (0x108000, "encryption"), (0x020000, "ethernet")):
            check(not matches(klass, cls(code), 0xFFFFFF00), f"{name}: {what} function would be claimed")

for path in ("dext/mac_linuxgpu.entitlements", "dext/Development.entitlements"):
    ent = load(path)
    for item in ent.get("com.apple.developer.driverkit.transport.pci", []):
        for key, spec in item.items():
            for value, mask in entries(spec, 0xFFFFFFFF):
                check(mask & 0xFFFF0000 == 0, f"{path}: {key} {spec} restricts the device ID")

# No device-specific literal anywhere in the matching/branding files.
device_literal = re.compile(r"(?i)(0x)?[0-9a-f]{4}1002\b|r9700|gfx12\d\d|navi\s?4\d|7551")
for path in ("dext/Info.plist", "dext/mac_linuxgpu.entitlements",
             "dext/Development.entitlements", "host/Info.plist"):
    text = open(path).read()
    hit = device_literal.search(re.sub(r"0x00001002", "", text))
    check(hit is None, f"{path}: device-specific literal {hit.group(0) if hit else ''!r}")

if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("device matching plists: passed")
PY
