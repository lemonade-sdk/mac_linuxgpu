# Vulkan on mac_linuxgpu: RADV

Mesa's RADV Vulkan driver, built for macOS, on the upstream amdgpu DRM
interface that mac_linuxgpu exposes through its Linux-file transport.
Applications reach the GPU through the Vulkan loader, as on Linux: first
llama.cpp's Vulkan backend, then games.

Status: RADV runs on the R9700. `vulkaninfo` lists the GPU, and
llama.cpp's `test-backend-ops` passes on it: ADD 103/103 and MUL_MAT
1132/1132, computed by the GPU and compared with the CPU. Offline, RADV and
llama.cpp's Vulkan backend also run on the CS fixture's software GPU.

```
 application (llama.cpp, vulkaninfo, ...)
        │  Vulkan
 Vulkan loader (Homebrew vulkan-loader or the LunarG SDK)
        │  ICD: build/radv/install/share/vulkan/icd.d/radeon_icd.json
 libvulkan_radeon.dylib     RADV + ACO, Mesa 26.2.4, two declared patches
        │  libdrm / libdrm_amdgpu API, ac_linux_drm's native path
 libdrm_mlg.dylib           libmlg_drm/libdrm: libdrm over libmlg_drm
        │  mlg_open / mlg_ioctl / mlg_mmap, Linux request numbers
 libmlg_drm.dylib           the Linux-file RPC (linuxu/headers/rt/lx_abi.h)
        │  IOKit user client type 2
 MacLinuxGPU dext           upstream DRM + amdgpu: render node, GEM, CS, syncobjs
```

## The seam: a libdrm for mac_linuxgpu

RADV talks to the kernel through three things: libdrm's core API
(`drmGetDevices2`, `drmGetVersion`, syncobjs, PRIME), libdrm_amdgpu (device,
buffer and GPU VA management, used by `ac_linux_drm.c`), and a handful of
system calls on the DRM descriptors (`open` and `stat` of the render node,
`mmap` of buffers, `ioctl` through `util/os_drm.h`).

Two places to cut were considered:

- **(a) a libdrm-compatible library.** Provide `xf86drm.h` and `amdgpu.h`
  over libmlg_drm, so RADV's stock amdgpu winsys and Mesa's native
  `ac_linux_drm.c` path run unchanged, issuing the same requests as on
  Linux.
- **(b) a new `ac_drm` backend**, next to the virtio native-context one:
  a third implementation of every `ac_drm_*` function.

(a) was chosen. `ac_linux_drm.c` already is a thin layer of Linux requests
over libdrm; a backend would duplicate its ~45 functions, and it would still
not cover what sits outside `ac_drm`: device enumeration in the Vulkan
runtime, syncobjs through `util_sync_provider`, RADV's own buffer mapping and
`stat` of nodes. A libdrm covers all of those at once, keeps RADV on its
most-tested path, needs no change to RADV's winsys, and is usable by other
libdrm clients. What a library cannot intercept are the system calls RADV
makes directly; those go through one Mesa header (`util/os_drm.h`), which
the second patch teaches to call libdrm when libdrm says its files are not
kernel files.

### DRM descriptors

The driver's files live in the dext's per-client Linux process; their
numbers mean nothing to macOS. libdrm-mlg represents each one (an opened
render node, and every syncobj, sync_file and dma-buf descriptor a request
returns) by a socket of the process (one end of a socket pair; the library
keeps the other end). So:

- `close`, `dup`, `fcntl(F_DUPFD_CLOEXEC)` and `fork` work on them as on any
  descriptor, from Mesa or from the application, with no patch;
- when the last copy is closed, the kept end reads end-of-file and a
  watcher thread closes the driver's file: the same lifetime a kernel file
  has, including descriptors an application closes after
  `vkGetSemaphoreFdKHR`;
- a proxy is recognized by its socket's inode, so a copy made by `dup` is
  recognized too;
- requests that carry descriptors (PRIME, syncobj handle/fd, fence to
  sync_file) have them translated in both directions, and `O_CLOEXEC` is
  translated to Linux's value.

`drmFileOpen`, `drmFileStat`, `drmFileMmap` and `drmFileMunmap` (declared
under `LIBDRM_FILE_OPS` in `xf86drm.h`) stand in for the system calls that
cannot reach the driver. libsync's `sync_wait`, `sync_merge` and
`sync_accumulate` work through temporary syncobjs (a merge moves both fences
to two points of a timeline syncobj and exports the later one).

## Mesa pin and patches

`third_party/mesa` is a sparse submodule of Mesa at tag **mesa-26.2.4**
(`96cb43121031992b85767f9c1be8f3f48e22b1d2`), the latest stable release when
pinned, declared in `patches/manifest.json` as an optional upstream (marked
`update = none` in `.gitmodules`, so a plain recursive clone does not fetch
it). The sparse set is what a RADV-only Meson build reads (about 60 MB).
Three patches:

| Patch | Files | What |
| --- | --- | --- |
| `patches/mesa/radv-macos-build.patch` | `meson.build`, `src/amd/addrlib/meson.build`, `src/amd/common/meson.build`, `src/amd/vulkan/meson.build` | Find libdrm through pkg-config on Darwin when RADV is built; select addrlib's portable type definitions (`HAVE_TSERVER`) instead of the Apple kernel driver's private header; link RADV with `-undefined dynamic_lookup` as KosmicKrisp does (Mach-O cannot leave the entrypoint tables' weak layer symbols undefined). |
| `patches/mesa/drm-file-ops.patch` | `src/util/os_drm.h`, `src/amd/vulkan/radv_device.c`, `src/amd/vulkan/radv_physical_device.c`, `src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c` | `drm_ioctl` uses `drmIoctl` when libdrm defines `LIBDRM_FILE_OPS`; new `os_drm_open/stat/mmap/munmap` wrappers; RADV's six call sites use them. Without `LIBDRM_FILE_OPS` nothing changes. |
| `patches/mesa/radv-apple-silicon-vram.patch` | `src/amd/vulkan/winsys/amdgpu/radv_amdgpu_winsys.c`, `src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c` | On Apple silicon, report no CPU-visible VRAM and place every buffer that is not `NO_CPU_ACCESS` in GTT (see [VRAM and the CPU on Apple silicon](#vram-and-the-cpu-on-apple-silicon)). |

`scripts/verify-upstream.py` (`make verify-source`) checks the Mesa pin,
sparse set and that the working tree is the pin plus exactly these patches,
once Mesa is checked out. `lib-dext` verifies only the Linux tree
(`make verify-linux`), so local Mesa work cannot block a driver build.

`third_party/llama.cpp` is pinned the same way at **b11379**
(`1537a0a8b2f8711d840878b0a0677ab2213c882c`), unpatched.

## VRAM and the CPU on Apple silicon

Apple silicon gives the CPU only Device memory attributes for PCIe BARs, even
for a mapping that asks for write combining. Aligned stores to a VRAM mapping
work, but unaligned ones take an alignment fault, and `memcpy` makes them. On
the R9700, RADV put its command buffers in CPU-visible VRAM (resizable BAR
makes all of it visible), and the first `vkCreateDevice` died with `SIGBUS`
(`EXC_ARM_DA_ALIGN`) in `memmove` writing the graphics preamble. A probe of
each mapping on the R9700:

| Mapping | aligned store | unaligned store | unaligned memcpy | memset 0 |
| --- | --- | --- | --- | --- |
| GTT, cached | ok | ok | ok | ok |
| GTT, USWC (write-combined) | ok | ok | ok | ok |
| VRAM through BAR0 (write-combined) | ok | SIGBUS | SIGBUS | ok |

No mapping option makes BAR space normal memory (and a cacheable mapping of
it is not safe to try), so the CPU must not write VRAM through a mapping.
`radv-apple-silicon-vram.patch` makes RADV's winsys report no CPU-visible
VRAM on Apple silicon (no `HOST_VISIBLE | DEVICE_LOCAL` memory type; all VRAM
is one device-local heap) and place every buffer the CPU may map in GTT:
command buffers, descriptor pools, shader arenas, upload buffers. The GPU
reads them from system memory over the link; buffers the GPU alone uses
(`NO_CPU_ACCESS`: images, device-local Vulkan memory, model weights) stay in
VRAM. The dext's own CPU access to VRAM already uses aligned word copies
(`rt/device_string.h`) for the same reason.

## Building

Tools: `brew install meson ninja pkg-config glslang cmake vulkan-loader
vulkan-headers shaderc`. Mesa's generators need Python's mako, PyYAML and
packaging; `scripts/build-radv.sh` creates a virtualenv with them in
`build/radv/venv` when `python3` lacks them.

```sh
make radv            # scripts/build-radv.sh: Mesa setup, libdrm-mlg, Meson, install
make llama-vulkan    # scripts/build-llama-vulkan.sh: llama.cpp with GGML_VULKAN
make test-libdrm-mlg # libdrm-mlg end to end on the fixture (part of make test)
make test-radv-offline  # RADV on the fixture (part of make test; skips if not built)
make test-llama-offline # llama.cpp on RADV on the fixture (part of make test; skips if not built)
```

`make radv` produces, in `build/radv/install`:

```
lib/libvulkan_radeon.dylib     RADV, ACO, no LLVM; links only the system and libdrm-mlg
lib/libdrm_mlg.dylib           libdrm + libdrm_amdgpu
lib/libmlg_drm.dylib           the Linux-file RPC client
share/vulkan/icd.d/radeon_icd.json
```

The tree is relocatable (`@loader_path` rpath, ICD path relative to the
JSON). Meson options: `-Dvulkan-drivers=amd -Dplatforms= -Dllvm=disabled`,
no GL, no video, no zstd or SPIRV-Tools, so nothing from Homebrew is linked.
Only headless WSI is built; there is no display or window-system surface yet.

To use it from any Vulkan program, point the loader at it alone (this also
keeps MoltenVK out of the picture):

```sh
scripts/llama-radv.sh vulkaninfo --summary
# which is
VK_DRIVER_FILES=$PWD/build/radv/install/share/vulkan/icd.d/radeon_icd.json vulkaninfo --summary
```

Without the driver attached, the loader reports that no GPU was found.

## What runs offline

`make test-radv-offline` (`vulkan/tests/test_radv_offline.c`) loads the ICD
as the loader does and runs it against the CS fixture device
(`linuxu/tests/cs_fixture.c`): unmodified upstream DRM, GEM, TTM, VM,
drm_sched, CS and syncobjs over a software GPU, reached through
libdrm-mlg and libmlg_drm's loopback transport. The fixture models an R9700
(gfx1201: 64 CUs, 4 SEs, `GB_ADDR_CONFIG`, firmware versions, from Mesa's
own gfx1201 profile) with a compute ring and an SDMA ring.

| Step | Result |
| --- | --- |
| `vkCreateInstance`, `vkEnumeratePhysicalDevices` | one device, "AMD Radeon AI Pro R9700 (RADV GFX1201)", Vulkan 1.4, PCI bus info |
| memory heaps and types, queue families | from `AMDGPU_INFO`; compute queue family (the fixture has no GFX ring) |
| `vkCreateDevice` | context, preamble, internal buffers in VRAM and GTT |
| buffer, host-visible memory, map | GEM create, GEM_VA, mmap |
| empty command buffer + fence | `AMDGPU_CS`, fence signaled |
| `vkCmdFillBuffer` + `vkCmdUpdateBuffer` | executed by the software GPU (CP DMA, WRITE_DATA); data checked through the mapping |
| compute pipeline | SPIR-V compiled by ACO for gfx1201 |
| `vkCmdDispatch` + timeline semaphore + fence | submitted, the dispatch packet reaches the compute engine, semaphore and fence signal. The software GPU runs no shaders, so the result is not checked. |
| teardown | every driver file released |

`make test-llama-offline` goes one step further: it brings the fixture up
inside llama.cpp's own `test-backend-ops` (a library inserted by
`scripts/llama-radv.sh` through `MLG_PRELOAD`), so llama.cpp's Vulkan backend
runs on RADV on the fixture. llama.cpp lists
`Vulkan0: AMD Radeon AI Pro R9700 (RADV GFX1201)` with fp16 dot2, bf16,
integer dot and `VK_KHR_cooperative_matrix`; in performance mode its ADD and
its Q4_0/Q4_K matrix multiplications (matrix-vector and matrix-matrix
pipelines) are compiled by ACO, allocated, submitted and fenced. The
reported speeds are meaningless (no shader runs). The same library works
with any llama.cpp tool for manual runs:

```sh
LLAMA_TEST_KEEP=/tmp/libmlg_fixture.dylib make test-llama-offline
MLG_PRELOAD=/tmp/libmlg_fixture.dylib scripts/llama-radv.sh test-backend-ops perf -b Vulkan0 -o ROPE
```

`make test-libdrm-mlg` checks libdrm-mlg on its own against the same
fixture: the device list, descriptors (dup, release on last close), syncobj
and sync_file descriptors, sync_file wait and merge, amdgpu device
deduplication, VA ranges, GTT and VRAM buffers, dma-buf export and import.

For the fixture this work added, behind `cs_fixture_model_driver_streams()`
(off for the other tests): DMA_DATA and driver RELEASE_MEM execution,
chained IBs, and counting (not running) dispatches and state packets. The
loopback transport now maps VRAM and scattered buffers by gathering their
pages with Mach VM, as the dext maps them through IOKit.

## Hardware test plan (R9700)

With the R9700 attached and the driver installed (`scripts/activate.sh`):

1. **Driver up.** `scripts/read-driver-log.py` shows the session ready; the
   CS self-test passes on the GPU (`scripts/drm-selftest.py`).
2. **Build.** `make radv llama-vulkan`.
3. **Enumerate.**
   `RADV_DEBUG=info scripts/llama-radv.sh vulkaninfo --summary` lists
   "AMD Radeon AI Pro R9700 (RADV GFX1201)", deviceID 0x7551, the PCI
   address from the registry, a graphics+compute queue family and a compute
   family (and an SDMA transfer family where RADV enables one). Save the
   `RADV_DEBUG=info` dump: it is ac_gpu_info's view of the real GPU, to
   compare with the fixture's.
4. **Compute correctness.** `scripts/llama-radv.sh test-backend-ops -b Vulkan0
   -o ADD`, then `-o MUL_MAT`, then the full run without `-o`: each operation
   is computed on the GPU and compared with the CPU. This is the first time
   shaders run, so it is the step that checks dispatch results the fixture
   cannot. All should pass.
5. **llama-bench.** Download a small model, for example
   `Qwen2.5-0.5B-Instruct-Q4_K_M.gguf` (about 0.4 GB) or
   `Llama-3.2-1B-Instruct-Q4_K_M.gguf`, then
   `scripts/llama-radv.sh llama-bench -m <model> -ngl 99 -p 512 -n 128`.
   Record pp512 and tg128 tokens/s, and compare with the same card on Linux
   (RADV) if available.
6. **Generation.** `scripts/llama-radv.sh llama-completion -m <model> -ngl 99
   -p "The capital of France is" -n 32` produces coherent text.
7. **Teardown.** After each run, `scripts/read-driver-log.py` shows the
   client's files and buffers released and no quarantined session.

When something fails, collect: `LIBDRM_MLG_DEBUG=1` (every refused request
and mapping, every descriptor's lifetime), `RADV_DEBUG=info,startup`,
`MESA_VK_ABORT_ON_ERROR=1` for a backtrace at the first error, and the
driver log. Do not kill the driver process (README, "Known limitations").

## Remaining gaps

- **WSI and display.** Only headless WSI. Presenting needs a macOS surface:
  either RADV's Metal-layer WSI (Mesa builds `wsi_common_metal` for
  KosmicKrisp) blitting from a linear image, or dma-buf/IOSurface sharing.
  There is no display engine in the dext (compute only), so this is the
  next step for Quake3e.
- **sync_file poll.** A sync_file descriptor is a socket that never becomes
  readable: `poll` on it times out instead of reporting the fence. Mesa's
  own waits use `sync_wait` (implemented through syncobjs) and syncobj
  waits, so RADV is unaffected; an application polling an exported
  sync_file is not. A transport `LX_POLL`, or a waiter that writes to the
  kept socket end when the fence signals, would make `poll` work.
- **External memory.** dma-buf export and import within the process work.
  `VK_EXT_external_memory_host` (userptr) cannot work, since the driver
  process cannot reach this process's pages: `amdgpu_create_bo_from_user_mem`
  fails with `ENOSYS`, but RADV still advertises the extension (Mesa sets
  `has_userptr` for every non-virtio device). Sharing buffers with other
  processes, or with Metal and IOSurface, is not designed yet.
- **Fixed CPU mappings.** `MAP_FIXED` mappings are refused, so
  `VK_EXT_map_memory_placed` (advertised) fails at map time.
- **eventfd.** `drmSyncobjEventfd` fails with `EOPNOTSUPP`
  (`CONFIG_EVENTFD=n` in the driver).
- **Offline coverage.** The fixture runs no shaders and has no GFX or
  graphics state, so dispatch results, graphics queues and SDMA transfer
  queues are first exercised on hardware.
- **Performance.** Every request and every buffer mapping is an IOKit call;
  RADV suballocates, so mappings are few, but submit and wait latency is
  untuned (doorbells and user queues are not mapped into the client).
