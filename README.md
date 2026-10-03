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

**[LemonSeed Engine](https://github.com/Geramy/LSE), Qwen3.8-27B Q4 with DFlash2
speculative decoding, on an AMD Radeon AI PRO R9700 over Thunderbolt 5
(MacBook Pro, Apple M5 Max), running on this driver:**

| | Measured |
|---|---:|
| Decode, warm (DFlash2, 74% draft acceptance) | **43.3 tok/s** |
| Prefill, 73-token prompt, warm | 60.0 tok/s |
| Operations that fell back to the CPU | **0** |

Speculative decoding speed scales with how many drafted tokens are accepted,
which depends on the text being generated. With the measured 80 ms per
speculative step, the same setup gives:

| Draft acceptance | 74% (measured) | 80% | 90% | 96% |
|---|---:|---:|---:|---:|
| Decode tok/s | 43 | ~52 | ~71 | ~87 |

These are first-run numbers. Nothing in the driver has been tuned for
throughput yet: each doorbell write and each completion poll still crosses
into the driver. Mapping doorbells into the client and using KFD events are
the next steps.

## What works

- **Upstream driver bring-up.** The unmodified `amdgpu_pci_probe` completes
  on the R9700, and every firmware image is loaded on demand by name.
- **KFD compute sessions.** There is one KFD process per client, each with its
  own GPUVM. Buffers go through `ALLOC_MEMORY_OF_GPU` / `MAP_MEMORY_TO_GPU`, and
  queues go through `CREATE_QUEUE` → MES `ADD_QUEUE`, exactly as on Linux.
- **HSA runtime.** `libhsa-runtime64.dylib` is installed with the driver, in
  `/usr/local/lib` and `/Library/MacAMDGPU/runtime`. HRX/Loom-based software
  such as LemonSeed Engine finds it without configuration.
- **Clean lifecycle.** Clients can come and go. A session closes through
  upstream removal, interrupt drain and endpoint isolation, and the driver
  stays reusable.
- **Observability.** A read-only observer reads the driver's retained log and
  session state at any time (`scripts/read-driver-log.py`). While compute runs,
  it also reads upstream's own telemetry: sysfs attributes such as
  `gpu_busy_percent`, `mem_busy_percent`, `gpu_metrics` and hwmon, and
  `AMDGPU_INFO`. That's the same data `amdgpu_top` reads on Linux, and what
  [amdgpu_mtopg](https://github.com/lemonade-sdk/amdgpu_mtopg) displays.
- **Nothing hard-coded to one GPU.** The driver matches by AMD vendor ID and
  display/compute PCI class, and upstream's PCI ID table and IP discovery
  decide what is supported. Queue layout, MQDs, firmware names, ISA and limits
  all come from the device.

## See it running

[amdgpu_mtopg](https://github.com/lemonade-sdk/amdgpu_mtopg) monitoring the
R9700 through this driver while LemonSeed Engine generates text: GPU load from
`GRBM_STATUS` samples, SMU clocks and power, hwmon temperatures and fan, and
VRAM in use by the model.

![amdgpu_mtopg monitoring an R9700 through mac_linuxgpu](assets/amdgpu_mtopg.png)

## How it works

```
 your app (LemonSeed, HRX/Loom, other HSA clients)
        │  HSA API
 libhsa-runtime64.dylib                 installed with the driver
        │  IOKit user client
 ┌──────┴──────────────────── DriverKit extension ────────────────────┐
 │  per-client KFD process: /dev/kfd + DRM render node, in-process    │
 │  upstream amdkfd + amdgpu + TTM + DRM + drm_sched   (unmodified)   │
 │  linuxu: the Linux kernel API in userspace                         │
 │    workqueues · timers · RCU · fences · mm/VMA · mmu notifiers ·   │
 │    sysfs · firmware loader · DMA through DART · MMIO · MSI-X        │
 └──────┬─────────────────────────────────────────────────────────────┘
        │  PCIDriverKit: BARs, DMA, interrupts
   AMD GPU over Thunderbolt
```

- **Upstream stays upstream.** The Linux sources come from a git submodule
  pinned to one Linux commit. The build carries **one** declared patch (a
  3-line buddy-allocator rollback fix) and two declared compile-time
  interventions. A verifier checks that nothing else differs from upstream.
- **linuxu is where the porting happens.** It provides Linux semantics for
  locking, workqueues, timers, RCU, DMA fences, memory management, notifiers,
  sysfs and the firmware loader, and the PCI, DMA, MMIO and interrupt seams
  over DriverKit.
- **Firmware** is fetched from linux-firmware at build time, verified against
  a lock file, and served to the driver on demand by name.

## Requirements

- An Apple Silicon Mac and an AMD GPU in a Thunderbolt enclosure
- Xcode with the DriverKit SDK (`xcode-select` pointing at Xcode)
- `python3`, `git` and `curl` (from the Xcode command line tools)
- `cmake` for the HSA runtime (`brew install cmake`)
- `ripgrep` for some test scripts (`brew install ripgrep`)
- To sign and install the driver: an Apple developer team with the DriverKit
  PCI and system-extension entitlements, and matching provisioning profiles

## Getting the source

There are two ways. Both end in the same tree.

**Option 1: the setup script (recommended).** It downloads only what the
build needs.

```sh
git clone https://github.com/lemonade-sdk/mac_linuxgpu.git
cd mac_linuxgpu
scripts/bootstrap.sh
```

`scripts/bootstrap.sh` does all one-time setup and is safe to re-run:

1. checks the tools listed above;
2. fetches the pinned Linux kernel into `third_party/linux` as a shallow,
   partial, sparse checkout of only the paths the build reads (about 70 MB
   downloaded, 600 MB on disk);
3. applies the patches in `patches/linux/`;
4. downloads the locked linux-firmware files into `build/firmware/` and
   verifies their hashes;
5. verifies the result and writes a stamp under `build/setup/`.

**Option 2: plain git.**

```sh
git clone --recurse-submodules https://github.com/lemonade-sdk/mac_linuxgpu.git
# or, in an existing clone:
git submodule update --init
```

This works, but git checks out the entire kernel. Measured: about 3.3 GB
downloaded and 1.7 GB on disk, against about 70 MB with the setup script. The
first `make` (or `scripts/bootstrap.sh`) then converts the checkout to the
sparse set the build uses, applies the patches and fetches the firmware.

To keep the kernel at the right commit when you pull updates, use
`git pull --recurse-submodules`, or set it once with:

```sh
git config submodule.recurse true
```

**Either way, you don't have to run setup by hand.** Every `make` target
(except `clean`, `distclean`, `hsa` and `hsa-test`) checks the setup stamp. If
the kernel checkout is missing, at the wrong commit, or unpatched, or the
firmware is missing, make runs `scripts/bootstrap.sh` first.
`scripts/activate.sh` and the Xcode project's "Build Linux KMD" phase both go
through make, so a fresh clone builds from any entry point.

## Building and installing

```sh
make lib-dext            # the DriverKit build of the driver library
make test                # the offline test suite (no GPU needed)
scripts/activate.sh      # build, sign, install and activate the driver
```

| Command | Result |
| --- | --- |
| `make` / `make lib` | `build/libmacamgdu.a`, host platform, used by the tests |
| `make lib-dext` | `build-dk/libmacamgdu-dk.a`, DriverKit platform |
| `make dext` | links the dext with make (unsigned unless `DIDENTITY` is set) |
| `make test` | the offline test suite |
| `make hsa`, `make hsa-test` | the HSA runtime (`build/hsa`) and its unit tests |
| `make verify-source` | checks the upstream tree against the pin and patch set |
| `make clean` | removes build outputs, keeps setup and firmware |
| `make distclean` | removes `build/` and `build-dk/` |

The Xcode project `mac_linuxgpu.xcodeproj` builds the signed dext
(`MacLinuxGPU`) and the host app (`MacLinuxGPUHost`). Its first build phase
runs `make lib-dext`, and the app bundles `build/firmware/amdgpu`.
`scripts/build-installer-dmg.sh` produces a signed disk image whose app
installs the driver, the HSA runtime and the firmware.

`scripts/activate.sh` builds everything, signs with Xcode automatic signing or
locally approved provisioning profiles, installs the app to `/Applications`,
installs the firmware and HSA runtime, and waits for the driver to attach. Set
`XCODE_TEAM_ID` to sign with your own team. See `scripts/activate.sh --help`.

### How the upstream sources are used

- `third_party/linux` is a git submodule of
  [torvalds/linux](https://github.com/torvalds/linux) pinned to
  `1f63dd8ca0dc05a8272bb8155f643c691d29bb11`. Its sparse checkout covers the
  amdgpu tree, the DRM core, TTM, the scheduler, a few `lib/` helpers and
  `include/`. Paths that differ only in letter case are excluded, so the
  checkout stays clean on case-insensitive APFS.
- `mk/upstream_sources.mk` lists every upstream `.c` file the build
  compiles. Nothing is globbed. The include paths in `mk/host_clang.mk` point
  straight into the submodule.
- `patches/linux/*.patch` are applied to the submodule working tree by the
  setup script, never committed inside it. Re-applying is detected and
  skipped. To change a patch, edit it, update its `sha256` in
  `patches/manifest.json`, and run `scripts/bootstrap.sh --reset-linux`.
- `scripts/verify-upstream.py` runs as `make verify-source` and before every
  `lib-dext`. It checks that:
  - the submodule is at the pin;
  - the sparse set matches and has no case collisions;
  - exactly the declared patches are applied and nothing else in the
    submodule is modified;
  - the few verbatim upstream headers kept in `linuxu/headers` still match;
  - the compile-time interventions match the declared list.
- **To move to a newer kernel:** update the submodule commit, `linux.pin` in
  `patches/manifest.json` and `mk/upstream_sources.mk`, then run
  `scripts/bootstrap.sh`.

### Firmware

GPU firmware is not stored in this repository. `firmware/firmware.lock` pins
a linux-firmware release and lists each needed file with its SHA-256 and
size. `scripts/fetch-firmware.sh` downloads them from
`gitlab.com/kernel-firmware/linux-firmware`, falling back to the
git.kernel.org mirror, and verifies every hash.

- `scripts/fetch-firmware.sh --add amdgpu/<file>.bin` fetches more files at
  the pinned release and adds them to the lock.
- `scripts/fetch-firmware.sh --all` fetches the whole amdgpu directory, for
  GPUs beyond the ones in the lock. Bundle it with
  `LINUX_FIRMWARE_DIR=build/firmware/full scripts/activate.sh`.

At run time the driver asks for firmware by name. The installed host
component serves it from
`/Library/Application Support/MacLinuxGPU/firmware/amdgpu`.

## Layout

```
dext/              DriverKit extension sources, Info.plist, entitlements
host/              host app: installer, activation, firmware servicer, CLI
hsa/               userspace HSA runtime (CMake) and its tests
linuxu/headers/    Linux kernel API headers for userspace
linuxu/src/        their implementation: memory, locking, PCI, DMA, ...
linuxu/tests/      unit and integration tests
mk/                build configuration: CONFIG table, flags, source list
patches/           patches/linux/*.patch and the upstream manifest
firmware/          firmware.lock (the files themselves are fetched)
scripts/           setup, firmware fetch, verifier, installers, tests
tools/             CONFIG table consistency checks
third_party/linux  pinned upstream Linux (submodule)
```

## Known limitations

- **Alpha.** Tested on one GPU, the Radeon AI PRO R9700 (`gfx1201`), over
  Thunderbolt 5. Other AMD GPUs supported by upstream `amdgpu` should match and
  probe, but are untested.
- **Don't kill the driver process.** When a DriverKit driver dies while it
  owns a PCI device, macOS runs PCI crash recovery, and that can panic the
  machine (`IOPCIFamily`). If `scripts/read-driver-log.py` reports a
  quarantined session, restart the Mac instead.
- **Compute only.** The display stack is not built.
- **No GPU reset recovery and no suspend/resume** over Thunderbolt yet.
- **One GPU per driver instance.**
- **No PCIe atomics over Thunderbolt.** Linux has the same limit with this card
  over Thunderbolt, and upstream's non-atomic firmware path is used.

## Related projects

- **[amdgpu_mtopg](https://github.com/lemonade-sdk/amdgpu_mtopg):** a live
  GPU monitor for macOS that reads this driver's telemetry. A signed and
  notarized download is on its releases page.
- **[LemonSeed Engine](https://github.com/Geramy/LSE):** LLM inference on AMD
  GPUs through HRX/Loom. On macOS it runs on this driver and uses the HSA
  runtime the driver installs.

## License

Original code in this repository is available under either the
[MIT license](LICENSES/MIT) or the [GNU GPL v2](LICENSES/GPL-2.0), at your
option (`SPDX-License-Identifier: MIT OR GPL-2.0-only`); see
[LICENSE](LICENSE). Upstream Linux code in `third_party/linux`, and files in
`linuxu/` that state a Linux-derived license, keep their own licenses.
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
