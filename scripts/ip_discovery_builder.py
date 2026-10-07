#!/usr/bin/env python3
"""Build an IP discovery binary (the table upstream's amdgpu_discovery.c reads)
from a JSON description of the device's IP blocks.

The JSON is what scripts/capture-r9700-fixtures.py records from upstream's
own exports of a running card (the ip_discovery sysfs tree and
AMDGPU_INFO_DEV_INFO), or a synthetic description for the test harness's
self-check. Format (all numbers decimal):

  {"format": 1,
   "dies": [{"die_id": 0,
             "ips": [{"hw_id": 11, "instance": 0, "major": 12, "minor": 0,
                      "revision": 1, "harvest": 0,
                      "base_addresses": [4800, ...]}, ...]}],
   "gc_info": {"num_se": 4, "num_wgp0_per_sa": 4, "num_wgp1_per_sa": 0,
               "num_rb_per_se": 4, "num_gl2c": 16, "num_gprs": 1536,
               "num_max_gs_thds": 32, "gs_table_depth": 32,
               "gsprim_buff_depth": 1792}}

The binary has the layout of include/discovery.h, version 1 header, an IP
discovery table of version 4 with 32-bit base addresses, and a GC table
(gc_info v1.0); checksums as amdgpu_discovery_init verifies them. What the
sysfs tree does not export is not invented: an IP's variant and
sub-revision are 0, and there is no harvest, VCN, MALL or NPS table.
"""
import json
import struct
import sys

BINARY_SIGNATURE = 0x28211407
DISCOVERY_TABLE_SIGNATURE = 0x53445049
GC_TABLE_ID = 0x4347
TOTAL_TABLES = 6
IP_DISCOVERY, GC = 0, 1
DISCOVERY_TMR_SIZE = 10 << 10      # amdgpu_discovery.h: the buffer upstream reads it into
BINARY_HEADER = struct.Struct("<IHHHH")          # signature, major, minor, checksum, size
TABLE_INFO = struct.Struct("<HHHH")              # offset, checksum, size, padding
IP_HEADER = struct.Struct("<IHHIH")              # signature, version, size, id, num_dies
DIE_INFO = struct.Struct("<HH")                  # die_id, die_offset
DIE_HEADER = struct.Struct("<HH")                # die_id, num_ips
IP_V4 = struct.Struct("<HBBBBBB")                # hw_id, instance, num_base, major, minor, rev, subrev|variant
GPU_INFO_HEADER = struct.Struct("<IHHI")         # table_id, major, minor, size
GC_INFO_V1_0 = ("num_se", "num_wgp0_per_sa", "num_wgp1_per_sa", "num_rb_per_se", "num_gl2c",
                "num_gprs", "num_max_gs_thds", "gs_table_depth", "gsprim_buff_depth",
                "scalar_data_cache_size_per_sqc", "scalar_data_cache_line_size",
                "tcc_size", "tcc_cache_line_size")


def checksum(data):
    return sum(data) & 0xffff


def build(description):
    if description.get("format") != 1:
        raise ValueError("unknown description format")
    dies = description["dies"]
    if not 1 <= len(dies) <= 16:
        raise ValueError(f"{len(dies)} dies: the table holds 1 to 16")
    header_bytes = BINARY_HEADER.size + TOTAL_TABLES * TABLE_INFO.size
    ip_table_offset = header_bytes
    ip_header_bytes = IP_HEADER.size + 16 * DIE_INFO.size + 2

    # The IP discovery table: header, then each die's header and IPs.
    body = bytearray()
    die_infos = []
    at = ip_table_offset + ip_header_bytes
    for die in dies:
        die_infos.append((die["die_id"], at))
        chunk = bytearray(DIE_HEADER.pack(die["die_id"], len(die["ips"])))
        for ip in die["ips"]:
            if ip.get("harvest", 0):
                raise ValueError(f"hw_id {ip['hw_id']} instance {ip['instance']} is harvested: "
                                 "a harvest table is not reconstructed")
            bases = ip["base_addresses"]
            if not 0 < len(bases) < 256 or any(not 0 <= b <= 0xffffffff for b in bases):
                raise ValueError(f"hw_id {ip['hw_id']}: base addresses must be 1 to 255 32-bit values")
            chunk += IP_V4.pack(ip["hw_id"], ip["instance"], len(bases), ip["major"],
                                ip["minor"], ip["revision"], 0)
            chunk += struct.pack(f"<{len(bases)}I", *bases)
        body += chunk
        at += len(chunk)
    ip_size = ip_header_bytes + len(body)
    ip_table = bytearray(IP_HEADER.pack(DISCOVERY_TABLE_SIGNATURE, 4, ip_size, 0, len(dies)))
    for die_id, offset in die_infos + [(0, 0)] * (16 - len(die_infos)):
        ip_table += DIE_INFO.pack(die_id, offset)
    ip_table += b"\0\0"          # v4: base_addr_64_bit = 0
    ip_table += body
    assert len(ip_table) == ip_size

    # The GC table (gc_info v1.0).
    gc = description["gc_info"]
    values = [int(gc.get(name, 0)) for name in GC_INFO_V1_0]
    gc_size = GPU_INFO_HEADER.size + 4 * len(values)
    gc_table = GPU_INFO_HEADER.pack(GC_TABLE_ID, 1, 0, gc_size) + struct.pack(f"<{len(values)}I", *values)
    gc_offset = ip_table_offset + ip_size

    total = gc_offset + gc_size
    if total > DISCOVERY_TMR_SIZE or total > 0xffff:
        raise ValueError(f"{total} bytes: more than upstream's {DISCOVERY_TMR_SIZE}-byte discovery buffer")
    tables = [(0, 0, 0)] * TOTAL_TABLES
    tables[IP_DISCOVERY] = (ip_table_offset, checksum(ip_table), ip_size)
    tables[GC] = (gc_offset, checksum(gc_table), gc_size)
    after_checksum = bytearray()
    after_checksum += struct.pack("<H", total)
    for offset, sum_, size in tables:
        after_checksum += TABLE_INFO.pack(offset, sum_, size, 0)
    after_checksum += ip_table + gc_table
    head = struct.pack("<IHH", BINARY_SIGNATURE, 1, 0)
    binary = head + struct.pack("<H", checksum(after_checksum)) + after_checksum
    assert len(binary) == total
    return bytes(binary)


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} DESCRIPTION.json OUT.bin", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as f:
        binary = build(json.load(f))
    with open(sys.argv[2], "wb") as f:
        f.write(binary)
    print(f"{sys.argv[2]}: {len(binary)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
