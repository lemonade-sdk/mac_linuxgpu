#!/usr/bin/env python3
"""Capture the card's IP discovery description and VBIOS for the deep session
cycle test (linuxu/tests/test_session_cycles.c, scripts/test-session-cycles.sh).

Read-only, through upstream's own export paths of the running driver; nothing
is written to the card:
  - the IP discovery table: upstream's ip_discovery sysfs tree (die/<die>/
    <hw_id>/<instance>/{major,minor,revision,harvest,base_addr}), read with
    the observer client, as scripts/read-sysfs.py reads sysfs;
  - the GC configuration for the table's GC section: AMDGPU_INFO_DEV_INFO,
    through the observer;
  - the VRAM size: upstream's mem_info_vram_total sysfs file;
  - the VBIOS: AMDGPU_INFO_VBIOS (VBIOS_SIZE, then VBIOS_IMAGE in chunks),
    upstream's copy of the image it fetched at probe, read on the render
    node through libmlg_drm;
  - the BAR sizes: the GPU's IOPCIDevice "assigned-addresses" in the I/O
    Registry (ioreg).

The driver must already be running in an open session: the observer reads
fail with "not ready" otherwise, and the capture stops before it opens the
render node (a Linux-file open of an uninitialized GPU would initialize it).

Output (default tests/fixtures/local/r9700/, which git ignores):
  ip_discovery.json  what was read, as recorded
  ip_discovery.bin   the discovery binary built from it (scripts/ip_discovery_builder.py)
  vbios.rom          the VBIOS image
The VBIOS is AMD's copyrighted image: these files stay local and are never
committed (the repository is public).
"""
import argparse
import ctypes as c
import importlib.util
import json
import os
import plistlib
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(REPO, "tests", "fixtures", "local", "r9700")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sysfs = load("read_sysfs", os.path.join(HERE, "read-sysfs.py"))
builder = load("ip_discovery_builder", os.path.join(HERE, "ip_discovery_builder.py"))

# include/uapi/drm/amdgpu_drm.h
AMDGPU_INFO_DEV_INFO = 0x16
AMDGPU_INFO_VBIOS = 0x1B
AMDGPU_INFO_VBIOS_SIZE, AMDGPU_INFO_VBIOS_IMAGE, AMDGPU_INFO_VBIOS_INFO = 1, 2, 3
DRM_COMMAND_BASE, DRM_AMDGPU_INFO = 0x40, 0x05
VBIOS_CHUNK = 4096


def linux_iow(nr, size):
    """DRM_IOW in Linux encoding (libmlg_drm takes Linux request numbers)."""
    return (1 << 30) | (size << 16) | (ord("d") << 8) | nr


class DrmAmdgpuInfo(c.Structure):
    _fields_ = [("return_pointer", c.c_uint64), ("return_size", c.c_uint32),
                ("query", c.c_uint32), ("type", c.c_uint32), ("offset", c.c_uint32),
                ("pad", c.c_uint32 * 2)]


assert c.sizeof(DrmAmdgpuInfo) == 32
DRM_IOCTL_AMDGPU_INFO = linux_iow(DRM_COMMAND_BASE + DRM_AMDGPU_INFO, c.sizeof(DrmAmdgpuInfo))


class DrmAmdgpuInfoDevice(c.Structure):
    """struct drm_amdgpu_info_device (natural alignment, as the uapi)."""
    _fields_ = [(n, t) for n, t in [
        ("device_id", c.c_uint32), ("chip_rev", c.c_uint32), ("external_rev", c.c_uint32),
        ("pci_rev", c.c_uint32), ("family", c.c_uint32), ("num_shader_engines", c.c_uint32),
        ("num_shader_arrays_per_engine", c.c_uint32), ("gpu_counter_freq", c.c_uint32),
        ("max_engine_clock", c.c_uint64), ("max_memory_clock", c.c_uint64),
        ("cu_active_number", c.c_uint32), ("cu_ao_mask", c.c_uint32),
        ("cu_bitmap", c.c_uint32 * 16), ("enabled_rb_pipes_mask", c.c_uint32),
        ("num_rb_pipes", c.c_uint32), ("num_hw_gfx_contexts", c.c_uint32), ("pcie_gen", c.c_uint32),
        ("ids_flags", c.c_uint64), ("virtual_address_offset", c.c_uint64),
        ("virtual_address_max", c.c_uint64), ("virtual_address_alignment", c.c_uint32),
        ("pte_fragment_size", c.c_uint32), ("gart_page_size", c.c_uint32), ("ce_ram_size", c.c_uint32),
        ("vram_type", c.c_uint32), ("vram_bit_width", c.c_uint32), ("vce_harvest_config", c.c_uint32),
        ("gc_double_offchip_lds_buf", c.c_uint32), ("prim_buf_gpu_addr", c.c_uint64),
        ("pos_buf_gpu_addr", c.c_uint64), ("cntl_sb_buf_gpu_addr", c.c_uint64),
        ("param_buf_gpu_addr", c.c_uint64), ("prim_buf_size", c.c_uint32), ("pos_buf_size", c.c_uint32),
        ("cntl_sb_buf_size", c.c_uint32), ("param_buf_size", c.c_uint32), ("wave_front_size", c.c_uint32),
        ("num_shader_visible_vgprs", c.c_uint32), ("num_cu_per_sh", c.c_uint32),
        ("num_tcc_blocks", c.c_uint32), ("gs_vgt_table_depth", c.c_uint32),
        ("gs_prim_buffer_depth", c.c_uint32), ("max_gs_waves_per_vgt", c.c_uint32),
        ("pcie_num_lanes", c.c_uint32), ("cu_ao_bitmap", c.c_uint32 * 16), ("high_va_offset", c.c_uint64),
        ("high_va_max", c.c_uint64), ("pa_sc_tile_steering_override", c.c_uint32),
        ("tcc_disabled_mask", c.c_uint64), ("min_engine_clock", c.c_uint64),
        ("min_memory_clock", c.c_uint64), ("tcp_cache_size", c.c_uint32), ("num_sqc_per_wgp", c.c_uint32),
        ("sqc_data_cache_size", c.c_uint32), ("sqc_inst_cache_size", c.c_uint32),
        ("gl1c_cache_size", c.c_uint32), ("gl2c_cache_size", c.c_uint32), ("mall_size", c.c_uint64),
        ("enabled_rb_pipes_mask_hi", c.c_uint32), ("shadow_size", c.c_uint32),
        ("shadow_alignment", c.c_uint32), ("csa_size", c.c_uint32), ("csa_alignment", c.c_uint32),
        ("userq_ip_mask", c.c_uint32), ("pad", c.c_uint32)]]


def numeric_dirs(call, path):
    return sorted(int(name) for kind, name in sysfs.list_dir(call, path)
                  if kind == "d" and name.isdigit())


def read_int(call, path):
    return int(sysfs.read_file(call, path).decode().split()[0], 0)


def capture_discovery(call):
    dies = []
    for die in numeric_dirs(call, "ip_discovery/die"):
        die_path = f"ip_discovery/die/{die}"
        ips = []
        for hw_id in numeric_dirs(call, die_path):
            for instance in numeric_dirs(call, f"{die_path}/{hw_id}"):
                at = f"{die_path}/{hw_id}/{instance}"
                bases = [int(word, 16) for word in sysfs.read_file(call, f"{at}/base_addr").decode().split()]
                if len(bases) != read_int(call, f"{at}/num_base_addresses"):
                    raise sysfs.DriverError(f"{at}: base_addr and num_base_addresses disagree")
                ips.append({"hw_id": read_int(call, f"{at}/hw_id"),
                            "instance": read_int(call, f"{at}/num_instance"),
                            "major": read_int(call, f"{at}/major"),
                            "minor": read_int(call, f"{at}/minor"),
                            "revision": read_int(call, f"{at}/revision"),
                            "harvest": read_int(call, f"{at}/harvest"),
                            "base_addresses": bases})
        if len(ips) != read_int(call, f"{die_path}/num_ips"):
            raise sysfs.DriverError(f"{die_path}: {len(ips)} IPs listed, num_ips says otherwise")
        dies.append({"die_id": die, "ips": ips})
    if not dies:
        raise sysfs.DriverError("ip_discovery/die lists no die")
    return dies


def capture_dev_info(call):
    raw = sysfs.drm_info(call, AMDGPU_INFO_DEV_INFO, b"", c.sizeof(DrmAmdgpuInfoDevice))
    info = DrmAmdgpuInfoDevice.from_buffer_copy(raw)
    return {name: (list(getattr(info, name)) if isinstance(getattr(info, name), c.Array)
                   else getattr(info, name)) for name, _ in info._fields_}


def gc_info_from(dev):
    """gc_info v1.0 from AMDGPU_INFO_DEV_INFO. A CU is half a WGP; RBs are
    counted per SE. GPRs per SIMD are not exported: the value recorded is the
    shader-visible VGPR count, which only sizes upstream's reporting."""
    se = dev["num_shader_engines"]
    if not se or dev["num_cu_per_sh"] % 2:
        raise ValueError("DEV_INFO: no shader engines, or an odd CU count per SA")
    return {"num_se": se, "num_wgp0_per_sa": dev["num_cu_per_sh"] // 2, "num_wgp1_per_sa": 0,
            "num_rb_per_se": dev["num_rb_pipes"] // se, "num_gl2c": dev["num_tcc_blocks"],
            "num_gprs": dev["num_shader_visible_vgprs"], "num_max_gs_thds": dev["max_gs_waves_per_vgt"],
            "gs_table_depth": dev["gs_vgt_table_depth"], "gsprim_buff_depth": dev["gs_prim_buffer_depth"],
            "sh_per_se": dev["num_shader_arrays_per_engine"]}


def bar_sizes(vendor, device):
    """BAR index -> size from the IOPCIDevice's assigned-addresses: per BAR
    five little-endian words, phys.hi (register number in bits 0-7, 0x10 +
    4 * BAR), then the address and the size as 64-bit low/high pairs."""
    out = subprocess.run(["ioreg", "-a", "-r", "-c", "IOPCIDevice", "-d", "1"], check=True,
                         capture_output=True).stdout
    for entry in plistlib.loads(out):
        vid, did = entry.get("vendor-id"), entry.get("device-id")
        if not vid or not did or struct.unpack_from("<H", vid)[0] != vendor or \
           struct.unpack_from("<H", did)[0] != device:
            continue
        sizes = {}
        cells = entry.get("assigned-addresses", b"")
        for at in range(0, len(cells) - 19, 20):
            hi, _, _, size_lo, size_hi = struct.unpack_from("<5I", cells, at)
            reg = hi & 0xff
            if 0x10 <= reg <= 0x24 and not (reg - 0x10) % 4:
                bar = (reg - 0x10) // 4
                sizes[bar] = (size_hi << 32) | size_lo
        return sizes
    raise RuntimeError(f"no IOPCIDevice {vendor:04x}:{device:04x} in the I/O Registry")


def capture_vbios(library):
    lib = c.CDLL(library)
    lib.mlg_open.argtypes, lib.mlg_open.restype = [c.c_char_p, c.c_int], c.c_int
    lib.mlg_close.argtypes, lib.mlg_close.restype = [c.c_int], c.c_int
    lib.mlg_ioctl.argtypes, lib.mlg_ioctl.restype = [c.c_int, c.c_ulong, c.c_void_p], c.c_int
    lib.mlg_last_linux_errno.restype = c.c_int
    fd = lib.mlg_open(b"/dev/dri/renderD128", os.O_RDWR)
    if fd < 0:
        raise OSError(lib.mlg_last_linux_errno(), "render node open failed")
    try:
        def query(kind, offset, size):
            buffer = c.create_string_buffer(size)
            request = DrmAmdgpuInfo(return_pointer=c.addressof(buffer), return_size=size,
                                    query=AMDGPU_INFO_VBIOS, type=kind, offset=offset)
            if lib.mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, c.byref(request)) != 0:
                code = lib.mlg_last_linux_errno()
                raise OSError(code, f"AMDGPU_INFO_VBIOS {kind} at {offset}: {os.strerror(code)}")
            return buffer.raw
        size, = struct.unpack("<I", query(AMDGPU_INFO_VBIOS_SIZE, 0, 4))
        if not size:
            raise RuntimeError("the driver holds no VBIOS image")
        image = bytearray()
        while len(image) < size:
            n = min(VBIOS_CHUNK, size - len(image))
            image += query(AMDGPU_INFO_VBIOS_IMAGE, len(image), n)[:n]
        info = query(AMDGPU_INFO_VBIOS_INFO, 0, 200)
    finally:
        lib.mlg_close(fd)
    if image[:2] != b"\x55\xaa":
        raise RuntimeError("the VBIOS image does not start with the ROM signature 55 AA")
    name, pn, version_str = (info[:64], info[64:128], info[136:168])
    text = lambda b: b.split(b"\0", 1)[0].decode(errors="replace")
    return bytes(image), {"name": text(name), "part_number": text(pn), "version": text(version_str)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=DEFAULT_OUT, help="output directory (default: %(default)s)")
    parser.add_argument("--libmlg-drm", default=os.environ.get("MLG_DRM_LIB", "/usr/local/lib/libmlg_drm.dylib"))
    parser.add_argument("--force", action="store_true", help="replace an existing capture")
    parser.add_argument("--rebuild", action="store_true",
                        help="only rebuild ip_discovery.bin from the recorded ip_discovery.json")
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    json_path, bin_path, rom_path = (os.path.join(out, n) for n in
                                     ("ip_discovery.json", "ip_discovery.bin", "vbios.rom"))
    if args.rebuild:
        with open(json_path) as f:
            binary = builder.build(json.load(f))
        with open(bin_path, "wb") as f:
            f.write(binary)
        print(f"{bin_path}: rebuilt, {len(binary)} bytes")
        return 0
    if not args.force and any(os.path.exists(p) for p in (json_path, bin_path, rom_path)):
        print(f"{out} already holds a capture (--force replaces it)", file=sys.stderr)
        return 1

    call, close = sysfs.connect()
    try:
        # Observer reads first: they fail unless the driver runs in an open
        # session, before anything opens the render node.
        dies = capture_discovery(call)
        dev = capture_dev_info(call)
        vram_total = read_int(call, "mem_info_vram_total")
    finally:
        close()
    image, vbios_info = capture_vbios(args.libmlg_drm)
    bars = bar_sizes(0x1002, dev["device_id"])
    if any(bar not in bars for bar in (0, 2, 5)):
        raise RuntimeError(f"the I/O Registry shows BARs {sorted(bars)}; the test needs 0, 2 and 5")

    description = {
        "format": 1,
        "source": "upstream exports of a running driver (scripts/capture-r9700-fixtures.py)",
        "device_id": dev["device_id"], "pci_rev": dev["pci_rev"], "family": dev["family"],
        "dies": dies, "gc_info": gc_info_from(dev), "vram_total_bytes": vram_total,
        "bar_sizes": {str(k): v for k, v in sorted(bars.items())},
        "vbios": dict(vbios_info, size=len(image)), "dev_info": dev,
    }
    binary = builder.build(description)
    os.makedirs(out, exist_ok=True)
    with open(json_path, "w") as f:
        json.dump(description, f, indent=1)
        f.write("\n")
    with open(bin_path, "wb") as f:
        f.write(binary)
    with open(rom_path, "wb") as f:
        f.write(image)
    ips = sum(len(d["ips"]) for d in dies)
    print(f"captured {len(dies)} die(s), {ips} IPs, VBIOS {vbios_info['part_number']} "
          f"({len(image)} bytes), VRAM {vram_total >> 20} MiB, BARs "
          + ", ".join(f"{k}: {v:#x}" for k, v in sorted(bars.items())))
    print(f"wrote {json_path}, {bin_path} ({len(binary)} bytes), {rom_path}")
    print("These files are local test fixtures: never commit them (the VBIOS is AMD's).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, sysfs.DriverError) as error:
        print(f"capture failed: {error}", file=sys.stderr)
        sys.exit(1)
