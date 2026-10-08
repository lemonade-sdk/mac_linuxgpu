<div align="center">

# mac_linuxgpu

**The upstream Linux `amdgpu` driver, unmodified, running on macOS.**

AMD Radeon GPUs over Thunderbolt on Apple Silicon Macs, driven by the same
`amdgpu` + `amdkfd` code Linux uses, inside a DriverKit system extension.

[![License: MIT OR GPL-2.0](https://img.shields.io/badge/license-MIT%20OR%20GPL--2.0-blue.svg)](LICENSE)
![Platform: macOS on Apple Silicon](https://img.shields.io/badge/platform-macOS%20%7C%20Apple%20Silicon-black.svg)
![Tested GPU: Radeon AI PRO R9700](https://img.shields.io/badge/tested-Radeon%20AI%20PRO%20R9700%20(gfx1201)-ED1C24.svg)
![Status: alpha](https://img.shields.io/badge/status-alpha-orange.svg)

**mac_linuxgpu** · [amdgpu_mtopg](https://github.com/lemonade-sdk/amdgpu_mtopg) · [LemonSeed Engine](https://github.com/Geramy/LSE)

</div>

---

Apple Silicon Macs have Thunderbolt 5 but no AMD GPU driver. **mac_linuxgpu**
runs the real Linux kernel driver instead of reimplementing one. It compiles
the upstream `amdgpu`, `amdkfd`, TTM, DRM core and GPU scheduler sources
(468 files from a pinned Linux commit, unmodified) against **linuxu**, a
userspace implementation of the Linux kernel APIs those sources need, and
hosts the result in a PCIDriverKit extension.

The GPU initializes the way it does on Linux: IP discovery, PSP, SMU, GMC,
GFX12, SDMA and MES. Compute works the way it does on Linux too. Every client
is a KFD process with its own GPU address space, and its HSA queues are
MES-scheduled user queues. The goal is that GPU software written for
Linux/ROCm runs on a Mac with nothing Mac-specific.

## Performance

**[LemonSeed Engine](https://github.com/Geramy/LSE) 0.5.8, Qwen3.8-27B Q4 (MLX)
with the Q8 DFlash2 draft and the Q8 MTP head, on an AMD Radeon AI PRO R9700 over
Thunderbolt 5 (MacBook Pro, Apple M5 Max), driver build 266 with the HSA runtime
the LSE archive bundles (build 267 provides it system-wide).** Measured from the
macOS release archive at temperature 0.6 with the defaults (adaptive DFlash2 with
draft trees, adaptive MTP=3, plain without MTP), median of three runs after a
warm-up, with draft acceptance and mean verify width in parentheses:

| Decode | DFlash2 | MTP=3 | Plain |
|---|---:|---:|---:|
| Code (HumanEval-style, 302 prompt tokens, 640 out) | **159.4 tok/s** (98%, 8.42) | 111.9 tok/s (97%, 5.94) | 32.2 tok/s |
| Essay (32 prompt tokens, 640 out) | 73.8 tok/s (75%, 14.86) | 56.9 tok/s (70%, 3.26) | 32.6 tok/s |
| 2K prompt (2,406 tokens, 256 out) | 82.1 tok/s (81%, 13.84) | 55.5 tok/s (68%, 4.51) | 32.2 tok/s |
| 4K prompt (4,786 tokens, 256 out) | 80.8 tok/s (81%, 14.45) | 60.1 tok/s (74%, 4.42) | 31.8 tok/s |

| Prefill (DFlash2 server, warm) | Time to first token | Prefill |
|---|---:|---:|
| 1,060 tokens | 0.662 s | **1,601 tok/s** |
| 2,118 tokens | 1.286 s | **1,646 tok/s** |
| 4,230 tokens | 2.588 s | **1,634 tok/s** |
| 33,799 tokens | 24.13 s | 1,401 tok/s |

Model load with the kernel cache on disk: 5.1 s with DFlash2, 4.9 s with MTP=3,
4.3 s plain. Server flags: `--pool hrx:0 --batch-size 1024 --ubatch-size 1024
--kv-cache-dtype bf16 --kv-len 262100`, with `LSE_REQUIRE_DEVICE_KERNELS=1`, so no
operation falls back to the CPU.

Firmware fetched at build time is distributed under its own terms (see its
`WHENCE` file) and is not part of this repository.

---

### LemonSeed Engine on the same GPU

This driver runs the stack behind these results. They are LemonSeed Engine's
HumanEval+ runs of Qwen3.8-27B Q4 on the AMD R9700 with HRX/Loom: 1,368
completed generations at standard, 16K and 32K context. They were measured
through the earlier [mac_amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)
driver path, and they are the target for this driver.

![LemonSeed Engine HumanEval+ on AMD R9700](https://raw.githubusercontent.com/Geramy/LSE/master/docs/benchmarks/flashprefill-humaneval-32k.png)
