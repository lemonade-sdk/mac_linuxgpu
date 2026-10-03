# Monitors on the AMD GPU as macOS displays

Status: design, with the first driver pieces implemented (STATUS and MODES
on selector 84, the hotplug epoch). It builds on the display test
(`linuxu/headers/rt/display.h`, selector 84). That test brings up Display
Core, detects every connector DC reads from the VBIOS and commits a
framebuffer through upstream KMS.

## Goal

Every monitor connected to the AMD GPU appears in System Settings ›
Displays as a real display. It has the monitor's name, vendor, product,
serial, physical size and modes. It can be arranged, mirrored or made the
main display, and macOS draws the desktop on it. Unplugging the monitor
removes the display. Changing a mode in Displays changes the monitor's
mode.

Nothing here depends on the connector type. DisplayPort, HDMI and USB-C
(DP alt mode) are all connectors that DC found. "Connected outputs" means
whatever DC reports as connected.

## Shape

```
 WindowServer (Apple GPU) draws a CGVirtualDisplay per monitor
        │  ScreenCaptureKit: an IOSurface per changed frame, dirty rects
 MacLinuxGPU display agent (host app / login agent)
        │  selector 84: STATUS (poll), PROBE, MODES, MODESET
        │  new: IMPORT (IOSurface pages), PRESENT (surface, rects)
 dext: per-output scanout context
        │  imported surface = GTT BO over the IOSurface pages (DART)
        │  SDMA copy of the dirty rects → back VRAM framebuffer
        │  atomic page flip on vblank (amdgpu_dm)
 R9700 scanout → monitor
```

## 1. Driver side

### 1a. Hotplug and connector state (implemented)

- **Monitor client.** The first STATUS call registers a DRM client
  (`drm_client_register`). Its `hotplug` callback counts events in a
  hotplug epoch. DM's HPD interrupt handler (`handle_hpd_irq` →
  `drm_kms_helper_connector_hotplug_event` → `drm_client_dev_hotplug`)
  reaches it. Upstream unregisters and frees the client in
  `drm_dev_unregister`. The callback takes no lock of ours.
- **STATUS** (op 3) returns the cached connector state with no detection
  and no AUX or DDC traffic, plus `hotplug_epoch`. The agent polls it
  every 250–500 ms, which costs one IOKit round trip. When the epoch
  changes, the agent runs PROBE (op 0), which does full detection.
- **MODES** (op 4) returns one connector's probed modes: size, refresh in
  mHz, pixel clock, and the preferred and interlace flags. It also
  returns the physical size from `display_info`.
- The EDID is read from sysfs (`drm/cardN/cardN-<conn>/edid`, selector 80).

Later: an async notification instead of polling. It would complete an
OSAction from the hotplug callback the way Linux-file async ioctls do.

### 1b. Scanout contexts (next)

There is one context per lit output. It holds 2 or 3 VRAM framebuffers
(dumb BOs through the same DRM client), the CRTC, the connector and the
current mode.

- **MODESET**(connector, mode index) commits the mode through
  `drm_client_modeset_commit`, as the display test does, with the
  context's front buffer.
- **Flip**: a non-blocking atomic commit that swaps the primary plane's
  framebuffer and completes on `flip_done` (the pflip interrupt). With
  no flip pending it costs nothing.
- Closing the context restores the configuration that was recorded
  before it, the same way the display test does.

### 1c. Frame import and present (needs hardware)

- **IMPORT**: the agent passes a captured IOSurface's pages as a
  structure-input memory descriptor. The surface is locked so its base
  address is valid; capture surfaces are linear BGRA. The dext retains the
  descriptor, prepares it for DMA (IODMACommand, DART IOVAs), and wraps
  the pages in an amdgpu GTT BO bound in the GART. That uses the same
  DART path GTT BOs already take. Each IOSurface is imported once, keyed
  by its IOSurfaceID: ScreenCaptureKit recycles a fixed pool of 3–8
  surfaces per stream. Imports are freed when the stream stops or the
  mode changes.
- **PRESENT**(import, connector, dirty rects):
  1. An SDMA copy of each dirty rect from the GTT import into the back
     framebuffer. That is `amdgpu_copy_buffer`-style linear copies per
     row band, or one sub-window copy per rect, on the kernel SDMA ring.
  2. Wait on the fence (bounded).
  3. Page flip.

  The Mac CPU does no per-pixel work. The GPU reads the surface over
  Thunderbolt DMA.
- **No fallbacks** (standing rule). If the import, DART mapping, copy,
  fence or flip fails, the call reports the failing step's errno. The
  agent stops that output's stream and reports the error. Nothing ever
  copies through the BAR with the CPU instead.
- **Budget.** A 4K BGRA surface is 33 MB. Four surfaces for each of two
  4K outputs is 265 MB of DART-mapped memory, which must fit within
  `linuxu_dart_budget()`. The agent sizes its SCK queue depth to fit and
  reports a refusal; it does not degrade silently.

## 2. Host side: the display agent

This is part of MacLinuxGPUHost, or a LaunchAgent it installs. It needs
Screen Recording permission (TCC) for ScreenCaptureKit. The private
CGVirtualDisplay API needs no entitlement; DeskPad and BetterDisplay use
it.

**CGVirtualDisplay on macOS 26.6.2.** The four classes exist in
CoreGraphics. I checked them with the ObjC runtime without creating
anything:

- `CGVirtualDisplayDescriptor`: `name`, `vendorID`, `productID`,
  `serialNum`/`serialNumber`, `sizeInMillimeters`, `maxPixelsWide/High`,
  red/green/blue primaries, `whitePoint`, `queue`, `terminationHandler`.
- `CGVirtualDisplay`: `initWithDescriptor:`, `applySettings:`,
  `displayID`, `hiDPI`, `rotation`, `modes`.
- `CGVirtualDisplaySettings`: `modes`, `hiDPI`, `rotation`,
  `refreshDeadline`, `isReference`.
- `CGVirtualDisplayMode`: `initWithWidth:height:refreshRate:`, with an
  optional `transferFunction`.

Per connected monitor (on a STATUS epoch change, then PROBE):

| CGVirtualDisplay | from |
|---|---|
| name | EDID monitor name descriptor (0xFC), else "<vendor> <product>" |
| vendorID / productID / serialNum | EDID PNP ID, product code, serial (or 0xFF text hash) |
| sizeInMillimeters | MODES width_mm × height_mm (EDID), else the preferred DTD's image size |
| primaries / whitePoint | EDID chromaticity (bytes 25–34) |
| maxPixelsWide/High | the largest MODES entry |
| settings.modes | MODES entries (progressive), preferred first, refresh from refresh_mhz |
| settings.hiDPI | 0 by default (see costs) |

On unplug the agent releases the CGVirtualDisplay, which removes it from
macOS, and closes the scanout context.

**Frames.** The agent runs one `SCStream` on the `SCDisplay` whose
`displayID` is the virtual display's, configured as:

- pixelFormat `kCVPixelFormatType_32BGRA`, matching DRM XRGB8888 with no
  conversion;
- width and height equal to the current mode;
- `minimumFrameInterval` = 1 / refresh;
- `queueDepth` 3–4;
- `showsCursor` on (a later step can use the hardware cursor plane).

For each complete frame, the agent takes the IOSurface (imported on first
sight) and `SCStreamFrameInfo.dirtyRects`, then calls PRESENT. On a static
desktop SCK delivers no frames, so nothing crosses the bus.

**Mode changes.** `CGDisplayRegisterReconfigurationCallback` watches the
virtual display's ID. When its mode changes, the agent calls MODESET with
the matching MODES entry, then restarts the stream at the new size and
drops the old imports.

## 3. What still costs the Mac, and how to keep it small

macOS still renders and composites the desktop for these displays on the
Apple GPU. The AMD GPU only scans out.

| Cost | Where | Keep it small |
|---|---|---|
| Compositing the virtual display | WindowServer, Apple GPU | Native mode at 1x (hiDPI off): a 2x HiDPI mode renders 4× the pixels, then downsamples |
| Capture | WindowServer blit into SCK IOSurfaces | 8-bit BGRA, frame interval = refresh, damage only |
| Per-frame call | agent CPU, one IOKit call (~10–30 µs) | one PRESENT per frame carrying all rects |
| Bus | Thunderbolt DMA reads by AMD SDMA | dirty rects only: 4K60 full frames are ~2 GB/s, of the ~10 GB/s TB5 carries |
| CPU copies | none: no BAR writes, no CPU pixel work | enforced: no fallback path exists |

**Measurement plan:**

- `powermetrics --samplers cpu_power,gpu_power` for Apple GPU and CPU
  power and residency, idle versus a moving window versus video.
- `top` / Activity Monitor for WindowServer and the agent.
- SCK frame statistics.
- Per-PRESENT driver timing (import, copy, fence, flip) in the retained
  log.
- `amdgpu_mtopg` for AMD SDMA load.

### 1d. What exists for the frame path (offline-tested)

- **The DMA mapping of a client's memory.** `dext_dma_import`
  (`iokit_bridge.mm`, `rt/dext_dma.h`) retains the memory descriptor the
  agent passed and prepares it with an IODMACommand on the GPU. It refuses
  segments that are not whole 16 KiB pages or that lie beyond the
  device's DMA width. It records the mapping in the same table as the
  driver's own coherent mappings, so the shutdown hold, the quarantine and
  fini rules all apply. Tested with the DriverKit mocks
  (`test-iokit-dma import`).
- **The GPU side.** `rt_surface_import` / `rt_surface_copy`
  (`rt/surface.h`) wrap those segments in a dma-buf exporter, import it
  through `amdgpu_gem_prime_import` (an SG BO pinned in GTT and bound in
  the GART) and copy dirty rectangles into a VRAM buffer with SDMA:
  - up to 1024 copy packets per job;
  - whole rows with equal pitches become one range;
  - the fence goes on both buffers.

  The provider's release, which calls `dext_dma_release_import`, runs only
  after the BO is destroyed, and TTM delays that until the copies'
  fences signal. `test-surface-import` runs this on the CS fixture, whose
  software SDMA engine reads system memory through the GART and faults on
  anything the DART does not map.
- **Wired (build 232):** selector 84 ops IMPORT, VERIFY, RELEASE, OUTPUT
  and PRESENT, for observer clients. A client's imports and the output it
  started end with its Stop; a session close releases them all.
  `MacLinuxGPUHost display-pin-test` answers the pinning question on
  hardware: it imports an IOSurface once, rewrites it from the CPU every
  second, and has the driver read it back with SDMA and through its own
  CPU view. `display-agent --create` mirrors one monitor: it creates a
  CGVirtualDisplay from the plan, lights the connector at the mode macOS
  picks (OUTPUT), captures the virtual display with ScreenCaptureKit, and
  sends each frame's damage with PRESENT, which does an SDMA copy and a
  flip on vblank. When macOS switches modes it relights. On exit it stops
  capture, releases the imports, restores the monitor and removes the
  display. It needs Screen Recording permission for the process that runs
  it.

### 1e. First hardware results (R9700 over TB5, MacBook Pro M5 Max, build 233)

- **Pinning.** `display-pin-test`: a 2560x1440 IOSurface (14.7 MB,
  pitch 10240) imported once. For 10 s the agent rewrote it every
  second, and each SDMA readback matched the new pattern exactly (0 of
  4096 sampled dwords different) and the previous pattern in none
  (4096 of 4096 different). Each readback took about 1.1 ms. The client's
  pages stay mapped for the device after the call returns.
  - The dext's own CPU mapping of the descriptor is a snapshot taken at
    the import call, so it is not used.
- **Mirroring.** `display-agent --create --seconds 60` on a DELL UP2716D:
  - the virtual display came online as display 6;
  - DP-4 was lit at 2560x1440 @ 59.950 Hz;
  - ScreenCaptureKit used a pool of 4 surfaces, each imported once;
  - 977 frames were captured and 976 presented, with an average SDMA
    copy of 2.94 ms and an average flip of 10.60 ms (the flip waits for
    vblank);
  - 2.2 GB was copied, and the agent used 0.55 s of CPU in 60 s;
  - teardown restored the monitor and removed the display, with no
    quarantine.

## 4. Increments

1. **Done on feature/display-c:** STATUS (cached state, hotplug epoch via
   the monitor client), MODES, display-test.py `status`/`modes`, and
   offline tests on the fixture DCN 4.0.1 device. The tests cover epoch
   1 at registration, epoch +1 on a hotplug event, the mode list of a
   connected output, and an empty list for an empty one.
2. **Done:** `MacLinuxGPUHost display-agent --dry-run [--once]
   [--interval ms] [--init]`. It polls STATUS and probes on an epoch
   change. For each connected monitor it reads MODES and the EDID and
   prints the CGVirtualDisplay it would create (name, vendor, product,
   serial, size, primaries, max pixels, modes with the preferred one
   first, hiDPI 0), then the add/update/remove changes. The model
   (`host/DisplayAgent.swift`) is tested by `test-display-agent`.
   Creating real displays needs the user's go-ahead, since they appear on
   the Mac's desktop. Without `--dry-run` the command refuses.
3. Scanout contexts and MODESET/flip, tested offline on the fixture
   device (register checks as in test-dm-offline).
4. IMPORT/PRESENT ops over the pieces in 1d. Hardware has to validate
   the DART mapping of another process's IOSurface pages (see the open
   questions) and SDMA reads over Thunderbolt.
5. Mode-change sync, measurements, then the async hotplug notification.

## 5. Open questions

- Can a DriverKit user client keep a structure-input memory descriptor
  past the call that passed it, and prepare it for DMA?
  `dext_dma_import` assumes it can: it retains the descriptor and
  PrepareForDMA wires it. Only a run on hardware can confirm this.
  - IOSurfaces that WindowServer allocates may be GPU-compressed or
    purgeable. The agent must pass surfaces it can lock for CPU access
    (SCK's BGRA output); if their pages cannot be prepared, the import
    fails with its error.
  - If it cannot be done, the agent maps dext-allocated GTT buffers
    instead (CopyClientMemoryForType) and has the Apple GPU blit the
    IOSurface into them with Metal (one small GPU copy). That would be a
    design change, decided on hardware results, never a silent fallback.
- What does the DART budget allow for N surfaces at 4K?
- SCK latency and frame pacing against AMD vblank. Do we need
  `refreshDeadline` on CGVirtualDisplaySettings?
- HDR and 10-bit (CGVirtualDisplayMode's transferFunction, an XRGB2101010
  scanout) come later.
