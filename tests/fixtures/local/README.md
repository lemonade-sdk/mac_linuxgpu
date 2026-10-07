# Local card captures

Files here are captured from a real card and are **never committed**: the
VBIOS is AMD's copyrighted image and this repository is public. `.gitignore`
excludes everything in this directory except this README.

## What uses them

`scripts/test-session-cycles.sh` (`make test-session-cycles`) runs its deep
session cycles from `r9700/` when the three files below are present, and
prints `SKIP deep session cycles` otherwise (as on CI). With the card's IP
discovery binary and VBIOS the real upstream probe gets past IP discovery and
the VBIOS into the IPs' initialization, so leaks on those paths (DMA, pages,
threads, heap) are caught by the cycle checks. A synthetic self-check of the
same harness runs on every invocation, with or without a capture.

`R9700_FIXTURES=<dir>` points the test at another capture directory;
`SESSION_CYCLES_DEEP=<n>` sets the number of deep cycles (default 200).

## Capturing

Prerequisites: the driver (build 243 or later) is installed and running in an
open session, with nothing else using the GPU that the capture could disturb.
Then, from the repository root:

    python3 scripts/capture-r9700-fixtures.py

It is read-only and uses upstream's own export paths of the running driver;
nothing is written to the card:

| File | Source |
|------|--------|
| `r9700/ip_discovery.json` | upstream's `ip_discovery` sysfs tree (observer client), `AMDGPU_INFO_DEV_INFO` (observer), `mem_info_vram_total` (sysfs), `AMDGPU_INFO_VBIOS_INFO`, BAR sizes from the I/O Registry |
| `r9700/ip_discovery.bin` | built from the JSON by `scripts/ip_discovery_builder.py` |
| `r9700/vbios.rom` | `AMDGPU_INFO_VBIOS` (`VBIOS_SIZE`, then `VBIOS_IMAGE` in chunks) on the render node through libmlg_drm: upstream's copy of the image it fetched at probe |

The observer reads come first and fail unless the driver is running, so the
render node is never opened on an uninitialized GPU (which would initialize
it). An existing capture is kept unless `--force` is given;
`--rebuild` regenerates `ip_discovery.bin` from the recorded JSON.

## What the discovery binary does not reproduce

The driver exports the discovery binary itself only in debugfs, which is not
reachable from user space, so the binary is rebuilt from upstream's sysfs
export of it. The sysfs tree does not show an IP's variant or sub-revision
(they are 0 here), and there is no harvest, VCN, MALL or NPS table (upstream
logs "invalid harvest table offset" and continues). The GC table is gc_info
v1.0 filled from `AMDGPU_INFO_DEV_INFO`; GPRs per SIMD are not exported, and
the shader-visible VGPR count stands in for them. The capture refuses a card
with harvested IPs rather than guess a harvest table.
