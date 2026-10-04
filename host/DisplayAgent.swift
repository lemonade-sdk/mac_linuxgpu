//
//  DisplayAgent.swift — the display agent's model: what the driver reports
//  about the GPU's outputs (selector 84, dext/sources/session_state.h), each
//  connected monitor's EDID, and the macOS virtual display that would stand
//  for it (docs/macos-displays.md). No IOKit, no ScreenCaptureKit and only
//  CoreGraphics geometry types, so it builds and is tested on its own
//  (scripts/test-display-agent.sh); DisplayAgentRuntime.swift drives it
//  over the observer client.
//

import Foundation
import CoreGraphics

// struct rt_display_report / rt_display_modes (linuxu/headers/rt/display.h),
// version 1.
let kDisplayReportBytes = 72 + 8 * 80
let kDisplayModesBytes = 24 + 56 * 16

enum DisplayOp: UInt64 {
    case probe = 0, show = 1, off = 2, status = 3, modes = 4
    case importSurface = 5, verify = 6, release = 7, output = 8, present = 9
}
let displayPatterns: [String: UInt64] = ["bars": 0, "white": 1, "gradient": 2]

private func le32(_ bytes: [UInt8], _ at: Int) -> UInt32 {
    UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
}

struct DisplayConnector {
    let name: String
    let id, status, modes, edidBytes: UInt32
    let preferredWidth, preferredHeight, preferredRefresh: UInt32
    let lit: Bool
    let litWidth, litHeight, litRefresh, crtc: UInt32

    var connected: Bool { status == 1 }
}

struct DisplayReport {
    let showing: Bool
    let pattern, fbWidth, fbHeight, fbPitch, crtcs: UInt32
    let fbGPUAddress, fillNs, commitNs: UInt64
    let probeStatus, commitStatus, restoreStatus: Int32
    let hotplugEpoch: UInt32
    let connectors: [DisplayConnector]

    init?(_ data: Data) {
        guard data.count >= kDisplayReportBytes else { return nil }
        let bytes = [UInt8](data)
        func u32(_ at: Int) -> UInt32 { le32(bytes, at) }
        func u64(_ at: Int) -> UInt64 { UInt64(u32(at)) | UInt64(u32(at + 4)) << 32 }
        guard u32(0) == 1, u32(4) <= 8 else { return nil }
        showing = u32(8) != 0
        pattern = u32(12); fbWidth = u32(16); fbHeight = u32(20); fbPitch = u32(24); crtcs = u32(28)
        fbGPUAddress = u64(32); fillNs = u64(40); commitNs = u64(48)
        probeStatus = Int32(bitPattern: u32(56)); commitStatus = Int32(bitPattern: u32(60))
        restoreStatus = Int32(bitPattern: u32(64))
        hotplugEpoch = u32(68)
        var list: [DisplayConnector] = []
        for index in 0..<Int(u32(4)) {
            let at = 72 + index * 80
            let raw = bytes[at..<at + 32].prefix { $0 != 0 }
            let f = (0..<12).map { u32(at + 32 + $0 * 4) }
            list.append(DisplayConnector(name: String(decoding: raw, as: UTF8.self), id: f[0], status: f[1],
                                         modes: f[2], edidBytes: f[3], preferredWidth: f[4],
                                         preferredHeight: f[5], preferredRefresh: f[6], lit: f[7] != 0,
                                         litWidth: f[8], litHeight: f[9], litRefresh: f[10], crtc: f[11]))
        }
        connectors = list
    }

    var lines: [String] {
        var out: [String] = []
        for c in connectors {
            let state = c.status == 1 ? "connected" : c.status == 2 ? "disconnected" : "unknown"
            var line = "  \(c.name.padding(toLength: 10, withPad: " ", startingAt: 0)) \(state)"
            if c.status == 1 {
                line += ", \(c.modes) mode(s), preferred \(c.preferredWidth)x\(c.preferredHeight)@\(c.preferredRefresh), EDID \(c.edidBytes) bytes"
            }
            if c.lit { line += "; showing \(c.litWidth)x\(c.litHeight)@\(c.litRefresh) on CRTC \(c.crtc)" }
            out.append(line)
        }
        out.append("  \(crtcs) CRTC(s)")
        if showing {
            let name = displayPatterns.first { $0.value == UInt64(pattern) }?.key ?? "\(pattern)"
            out.append(String(format: "  pattern %@: framebuffer %ux%u (pitch %u) at VRAM 0x%llx, written in %.1f ms, committed in %.1f ms",
                              name, fbWidth, fbHeight, fbPitch, fbGPUAddress,
                              Double(fillNs) / 1e6, Double(commitNs) / 1e6))
        }
        return out
    }
}

/// One connector's probed modes (struct rt_display_modes).
struct DisplayModes {
    struct Mode: Equatable {
        let width, height: Int
        let refreshMilliHz: Int
        let clockKHz: Int
        let preferred, interlaced: Bool
        var refresh: Double { Double(refreshMilliHz) / 1000 }
    }
    let status: UInt32
    let total: Int
    let widthMm, heightMm: Int
    let modes: [Mode]

    init?(_ data: Data) {
        guard data.count >= kDisplayModesBytes else { return nil }
        let bytes = [UInt8](data)
        let count = Int(le32(bytes, 8))
        guard le32(bytes, 0) == 1, count <= 56, count <= Int(le32(bytes, 12)) else { return nil }
        status = le32(bytes, 4)
        total = Int(le32(bytes, 12))
        widthMm = Int(le32(bytes, 16)); heightMm = Int(le32(bytes, 20))
        modes = (0..<count).map { i in
            let at = 24 + i * 16
            let flags = le32(bytes, at + 12)
            return Mode(width: Int(UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8),
                        height: Int(UInt16(bytes[at + 2]) | UInt16(bytes[at + 3]) << 8),
                        refreshMilliHz: Int(le32(bytes, at + 4)), clockKHz: Int(le32(bytes, at + 8)),
                        preferred: flags & 1 != 0, interlaced: flags & 2 != 0)
        }
    }
}

/// A Linux errno as the display test reports it.
func displayErrno(_ status: Int64) -> String {
    if status == 0 { return "ok" }
    let code = Int32(truncatingIfNeeded: -status)
    let meaning: String
    switch code {
    case ENODEV: meaning = ": no display (the driver runs without Display Core; install with activate.sh --display)"
    case ENOENT: meaning = ": no connected output (or no connector by that name)"
    case E2BIG: meaning = ": the outputs' modes need a framebuffer larger than 8192 pixels"
    default: meaning = ""
    }
    return "Linux errno \(code) (\(String(cString: strerror(code)))\(meaning))"
}

/// The identity, size, colour primaries and preferred timing of an EDID
/// base block (EDID 1.3/1.4).
struct EDIDSummary {
    let manufacturerID: UInt16     // the PNP ID word, as macOS reports a display's vendor
    let vendor: String
    let product: UInt16
    let serial: UInt32
    let name: String?
    let serialText: String?
    let year: Int
    let widthCm, heightCm: Int
    /// CIE xy of red, green, blue and white (10-bit fractions).
    let red, green, blue, white: (x: Double, y: Double)
    let preferred: (width: Int, height: Int, refresh: Double, clockHz: Int, widthMm: Int, heightMm: Int)?

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 128, b[0..<8] == [0, 255, 255, 255, 255, 255, 255, 0][...],
              b[0..<128].reduce(UInt8(0), &+) == 0 else { return nil }
        let word = Int(b[8]) << 8 | Int(b[9])
        manufacturerID = UInt16(word)
        vendor = String([10, 5, 0].map { Character(UnicodeScalar(UInt8(((word >> $0) & 31) + 64))) })
        product = UInt16(b[10]) | UInt16(b[11]) << 8
        serial = UInt32(b[12]) | UInt32(b[13]) << 8 | UInt32(b[14]) << 16 | UInt32(b[15]) << 24
        year = Int(b[17]) + 1990
        widthCm = Int(b[21]); heightCm = Int(b[22])
        // Bytes 25-26 carry the two low bits of each coordinate, 27-34 the high eight.
        func coordinate(_ high: Int, _ lowByte: Int, _ shift: Int) -> Double {
            Double(Int(b[high]) << 2 | (Int(b[lowByte]) >> shift) & 3) / 1024
        }
        red = (coordinate(27, 25, 6), coordinate(28, 25, 4))
        green = (coordinate(29, 25, 2), coordinate(30, 25, 0))
        blue = (coordinate(31, 26, 6), coordinate(32, 26, 4))
        white = (coordinate(33, 26, 2), coordinate(34, 26, 0))
        var name: String?, serialText: String?
        var preferred: (Int, Int, Double, Int, Int, Int)?
        for at in stride(from: 54, to: 126, by: 18) {
            let d = Array(b[at..<at + 18])
            if d[0] != 0 || d[1] != 0 {
                if preferred == nil {
                    let clock = (Int(d[0]) | Int(d[1]) << 8) * 10_000
                    let h = Int(d[2]) | Int(d[4] & 0xf0) << 4, hb = Int(d[3]) | Int(d[4] & 0x0f) << 8
                    let v = Int(d[5]) | Int(d[7] & 0xf0) << 4, vb = Int(d[6]) | Int(d[7] & 0x0f) << 8
                    let total = (h + hb) * (v + vb)
                    preferred = (h, v, total > 0 ? Double(clock) / Double(total) : 0, clock,
                                 Int(d[12]) | Int(d[14] & 0xf0) << 4, Int(d[13]) | Int(d[14] & 0x0f) << 8)
                }
                continue
            }
            let text = String(decoding: d[5..<18].prefix { $0 != 0x0a }, as: UTF8.self)
                .trimmingCharacters(in: .whitespaces)
            if d[3] == 0xfc { name = text } else if d[3] == 0xff { serialText = text }
        }
        self.name = name
        self.serialText = serialText
        self.preferred = preferred.map { (width: $0.0, height: $0.1, refresh: $0.2, clockHz: $0.3,
                                          widthMm: $0.4, heightMm: $0.5) }
    }

    var summary: String {
        var parts = [String(format: "%@ product 0x%04x", vendor, product),
                     name.map { "name '\($0)'" } ?? "no name",
                     "serial \(serialText ?? String(serial))", "made \(year)", "\(widthCm)x\(heightCm) cm"]
        if let p = preferred {
            parts.append(String(format: "preferred %dx%d@%.2f (%.2f MHz, %dx%d mm)", p.width, p.height,
                                p.refresh, Double(p.clockHz) / 1e6, p.widthMm, p.heightMm))
        }
        return parts.joined(separator: ", ")
    }
}

// ----------------------------------------------------------------
// MARK: - The virtual display a connected monitor becomes
// ----------------------------------------------------------------

/// What a CGVirtualDisplayDescriptor and its CGVirtualDisplaySettings would
/// carry for one monitor (property names as macOS 26 has them).
struct VirtualDisplayPlan: Equatable {
    struct Mode: Equatable {
        let width, height: Int
        let refreshRate: Double
    }
    let connector: String
    let name: String
    let vendorID: UInt32
    let productID: UInt32
    let serialNum: UInt32
    let sizeInMillimeters: (width: Double, height: Double)
    let maxPixelsWide, maxPixelsHigh: Int
    let redPrimary, greenPrimary, bluePrimary, whitePoint: (x: Double, y: Double)
    let modes: [Mode]              // preferred first
    let hiDPI: Int                 // 0: 1x, the cheapest for WindowServer

    static func == (a: VirtualDisplayPlan, b: VirtualDisplayPlan) -> Bool {
        a.connector == b.connector && a.name == b.name && a.vendorID == b.vendorID &&
            a.productID == b.productID && a.serialNum == b.serialNum &&
            a.sizeInMillimeters == b.sizeInMillimeters && a.maxPixelsWide == b.maxPixelsWide &&
            a.maxPixelsHigh == b.maxPixelsHigh && a.redPrimary == b.redPrimary &&
            a.greenPrimary == b.greenPrimary && a.bluePrimary == b.bluePrimary &&
            a.whitePoint == b.whitePoint && a.modes == b.modes && a.hiDPI == b.hiDPI
    }

    var lines: [String] {
        var out = [
            "  CGVirtualDisplayDescriptor for \(connector):",
            "    name \"\(name)\"",
            String(format: "    vendorID 0x%04x  productID 0x%04x  serialNum %u", vendorID, productID, serialNum),
            String(format: "    sizeInMillimeters %.0f x %.0f  maxPixels %d x %d", sizeInMillimeters.width,
                   sizeInMillimeters.height, maxPixelsWide, maxPixelsHigh),
            String(format: "    primaries R(%.4f, %.4f) G(%.4f, %.4f) B(%.4f, %.4f) white(%.4f, %.4f)",
                   redPrimary.x, redPrimary.y, greenPrimary.x, greenPrimary.y, bluePrimary.x, bluePrimary.y,
                   whitePoint.x, whitePoint.y),
            "  CGVirtualDisplaySettings: hiDPI \(hiDPI), \(modes.count) mode(s):",
        ]
        out += modes.map { String(format: "    %dx%d @ %.3f Hz", $0.width, $0.height, $0.refreshRate) }
        return out
    }
}

enum PlanError: Error, Equatable, CustomStringConvertible {
    case notConnected
    case noEDID
    case noProgressiveMode

    var description: String {
        switch self {
        case .notConnected: return "not connected"
        case .noEDID: return "no valid EDID (a monitor without one cannot be described to macOS)"
        case .noProgressiveMode: return "no progressive mode"
        }
    }
}

/// The plan for one connected monitor. The EDID gives the identity and
/// colour; the driver's mode list (what upstream accepted for this link)
/// gives the modes; nothing is invented.
func planVirtualDisplay(connector: String, modes: DisplayModes, edid: Data?) -> Result<VirtualDisplayPlan, PlanError> {
    guard modes.status == 1 else { return .failure(.notConnected) }
    guard let edid, let info = EDIDSummary(edid) else { return .failure(.noEDID) }
    var seen = Set<String>()
    var list: [VirtualDisplayPlan.Mode] = []
    // Preferred first, then the driver's order (upstream sorts by size, then refresh).
    for mode in modes.modes.filter({ $0.preferred }) + modes.modes.filter({ !$0.preferred })
    where !mode.interlaced && mode.width > 0 && mode.height > 0 && mode.refreshMilliHz > 0 {
        let key = "\(mode.width)x\(mode.height)@\(mode.refreshMilliHz / 10)"
        if seen.insert(key).inserted {
            list.append(.init(width: mode.width, height: mode.height, refreshRate: mode.refresh))
        }
    }
    guard !list.isEmpty else { return .failure(.noProgressiveMode) }
    let name = info.name.flatMap { $0.isEmpty ? nil : $0 } ?? String(format: "%@ %04X", info.vendor, info.product)
    // A numeric serial when the EDID has one; else a stable hash of its
    // serial string; else 0.
    var serial = info.serial
    if serial == 0, let text = info.serialText, !text.isEmpty {
        serial = text.utf8.reduce(UInt32(2166136261)) { ($0 ^ UInt32($1)) &* 16777619 }
    }
    let size: (Double, Double) = modes.widthMm > 0 && modes.heightMm > 0 ?
        (Double(modes.widthMm), Double(modes.heightMm)) : (Double(info.widthCm * 10), Double(info.heightCm * 10))
    return .success(VirtualDisplayPlan(
        connector: connector, name: name, vendorID: UInt32(info.manufacturerID),
        productID: UInt32(info.product), serialNum: serial, sizeInMillimeters: size,
        maxPixelsWide: list.map(\.width).max() ?? 0, maxPixelsHigh: list.map(\.height).max() ?? 0,
        redPrimary: info.red, greenPrimary: info.green, bluePrimary: info.blue, whitePoint: info.white,
        modes: list, hiDPI: 0))
}

/// The agent's view of the displays it maintains: the plans it acted on,
/// by connector, and what a new probe changes.
struct DisplayAgentState {
    enum Change: Equatable, CustomStringConvertible {
        case add(VirtualDisplayPlan)
        case update(VirtualDisplayPlan)
        case remove(String)
        case unchanged(String)

        var description: String {
            switch self {
            case .add(let p): return "add \(p.connector) (\"\(p.name)\")"
            case .update(let p): return "update \(p.connector) (\"\(p.name)\")"
            case .remove(let c): return "remove \(c)"
            case .unchanged(let c): return "unchanged \(c)"
            }
        }
    }

    private(set) var displays: [String: VirtualDisplayPlan] = [:]
    private(set) var epoch: UInt32?

    /// Whether the driver's hotplug epoch says the outputs need a new probe.
    mutating func needsProbe(epoch now: UInt32) -> Bool {
        defer { epoch = now }
        return epoch != now
    }

    /// Apply the plans of the currently connected monitors (connectors that
    /// failed to plan are absent): the changes, in connector order.
    mutating func apply(_ plans: [VirtualDisplayPlan]) -> [Change] {
        var changes: [Change] = []
        let incoming = Dictionary(plans.map { ($0.connector, $0) }, uniquingKeysWith: { a, _ in a })
        for connector in Set(displays.keys).union(incoming.keys).sorted() {
            switch (displays[connector], incoming[connector]) {
            case (nil, let plan?): changes.append(.add(plan))
            case (_?, nil): changes.append(.remove(connector))
            case (let old?, let plan?): changes.append(old == plan ? .unchanged(connector) : .update(plan))
            default: break
            }
        }
        displays = incoming
        return changes
    }
}

// ----------------------------------------------------------------
// MARK: - Frames: the pinning check and PRESENT replies
// ----------------------------------------------------------------

/// rt_surface_pattern (linuxu/headers/rt/surface.h): the value of dword
/// @index of a surface filled for @seed.
func surfacePattern(seed: UInt32, dword index: UInt64) -> UInt32 {
    UInt32(truncatingIfNeeded: index &* 2654435761) ^ (seed &* 0x85ebca77) ^ 0x5a5a0000
}

/// struct rt_surface_verify_result, version 1.
struct SurfaceVerifyResult {
    static let bytes = 64
    let samples, sampleBytes, gpuMismatches, cpuMismatches: UInt32
    let cpuChecked: Bool
    let firstGPUMismatch, firstCPUMismatch, gpuNs, gpuAddress: UInt64
    let gpuValue, expectedValue: UInt32

    var dwords: UInt32 { samples * sampleBytes / 4 }

    init?(_ data: Data) {
        guard data.count >= SurfaceVerifyResult.bytes else { return nil }
        let b = [UInt8](data)
        func u64(_ at: Int) -> UInt64 { UInt64(le32(b, at)) | UInt64(le32(b, at + 4)) << 32 }
        guard le32(b, 0) == 1 else { return nil }
        samples = le32(b, 4); sampleBytes = le32(b, 8)
        gpuMismatches = le32(b, 12); cpuMismatches = le32(b, 16); cpuChecked = le32(b, 20) != 0
        firstGPUMismatch = u64(24); firstCPUMismatch = u64(32); gpuNs = u64(40); gpuAddress = u64(48)
        gpuValue = le32(b, 56); expectedValue = le32(b, 60)
    }
}

/// struct rt_display_present_stats, version 1.
/// struct rt_display_present_stats, version 3 (linuxu/headers/rt/display.h;
/// version 2, 120 bytes, without the VRAM counters, from a driver before 240):
/// the output worker's counters since OUTPUT. PRESENT returns them as they
/// stand when it queues a frame (the frame itself is copied and flipped
/// later); PRESENT with no rectangle only reads them.
struct PresentStats: Equatable {
    static let bytes = 144
    let engines: UInt32
    let received, flipped, replaced, copyJobs, bytesCopied: UInt64
    let copySubmitNs, copyGPUNs, latencyNs, latencyMaxNs: UInt64
    let lastBytes, lastCopyGPUNs, lastLatencyNs: UInt64
    let error: Int32
    let fullFrames: UInt32
    /// CPU accesses to the VRAM aperture (all users): what a power-off
    /// could catch in flight (the driver gates them once it knows).
    let apertureOps: UInt64
    /// Version 3: bytes copied from the framebuffer drawn before (in VRAM;
    /// bytesCopied came over PCIe), rows moved there, moves copied from the
    /// surface instead (the frame before was replaced in the mailbox).
    let vramBytes, movedRows, movesDropped: UInt64

    init?(_ data: Data) {
        guard data.count >= 120 else { return nil }
        let b = [UInt8](data)
        func u64(_ at: Int) -> UInt64 { UInt64(le32(b, at)) | UInt64(le32(b, at + 4)) << 32 }
        let version = le32(b, 0)
        guard version == 2 || (version == 3 && data.count >= PresentStats.bytes) else { return nil }
        engines = le32(b, 4)
        received = u64(8); flipped = u64(16); replaced = u64(24); copyJobs = u64(32); bytesCopied = u64(40)
        copySubmitNs = u64(48); copyGPUNs = u64(56); latencyNs = u64(64); latencyMaxNs = u64(72)
        lastBytes = u64(80); lastCopyGPUNs = u64(88); lastLatencyNs = u64(96)
        error = Int32(bitPattern: le32(b, 104)); fullFrames = le32(b, 108)
        apertureOps = u64(112)
        if version == 3 {
            vramBytes = u64(120); movedRows = u64(128); movesDropped = u64(136)
        } else {
            vramBytes = 0; movedRows = 0; movesDropped = 0
        }
    }
}

/// What one mirroring run measured: the agent's side per presented frame
/// (the PRESENT call's CPU and wall time, the whole frame handler's CPU)
/// and the driver's per flipped frame (copy time against the bytes copied),
/// summarized against the driver's counters at the start and the end of the
/// measured window.
struct PresentMeasurement {
    /// Copy time by damage size: under 64 KiB, under 1 MiB, under 4 MiB, more.
    static let bucketLimits: [UInt64] = [64 << 10, 1 << 20, 4 << 20, .max]
    static let bucketNames = ["<64K", "64K-1M", "1M-4M", ">=4M"]
    var frames = 0, idleFrames = 0, presented = 0
    var callCPUNs: UInt64 = 0, callWallNs: UInt64 = 0, callWallMaxNs: UInt64 = 0, handlerCPUNs: UInt64 = 0
    var bucketFrames = [Int](repeating: 0, count: 4)
    var bucketBytes = [UInt64](repeating: 0, count: 4)
    var bucketCopyNs = [UInt64](repeating: 0, count: 4)
    var lastFlipped: UInt64 = 0

    /// Damage ScreenCaptureKit reported and what was left once unchanged
    /// tiles were dropped (bytes), frames left with none, hashing time.
    var reportedBytes: UInt64 = 0, keptBytes: UInt64 = 0, unchangedFrames = 0, filterNs: UInt64 = 0, filterFrames = 0
    /// Bytes the moves cover (copied in VRAM rather than over PCIe), frames with a move.
    var movedBytes: UInt64 = 0, movedFrames = 0

    mutating func filtered(reported: [DamageFilter.Rect], kept: DamageFilter.Damage, ns: UInt64) {
        func bytes(_ r: [DamageFilter.Rect]) -> UInt64 { r.reduce(0) { $0 + UInt64($1.w) * UInt64($1.h) * 4 } }
        reportedBytes += bytes(reported)
        keptBytes += bytes(kept.rects)
        movedBytes += kept.moves.reduce(0) { $0 + UInt64($1.w) * UInt64($1.h) * 4 }
        movedFrames += kept.moves.isEmpty ? 0 : 1
        unchangedFrames += kept.isEmpty ? 1 : 0
        filterNs += ns
        filterFrames += 1
    }

    /// From a frame's composition (its display time) to the frame handler.
    var lagNs: UInt64 = 0, lagMaxNs: UInt64 = 0, lagFrames = 0

    mutating func delivered(lagNs: UInt64) {
        self.lagNs += lagNs
        lagMaxNs = max(lagMaxNs, lagNs)
        lagFrames += 1
    }

    /// Waiting for the compositor to finish writing the captured buffer.
    var lockWaitNs: UInt64 = 0, lockWaitMaxNs: UInt64 = 0

    mutating func present(callCPUNs: UInt64, callWallNs: UInt64, handlerCPUNs: UInt64, lockWaitNs: UInt64 = 0) {
        presented += 1
        self.lockWaitNs += lockWaitNs
        lockWaitMaxNs = max(lockWaitMaxNs, lockWaitNs)
        self.callCPUNs += callCPUNs
        self.callWallNs += callWallNs
        callWallMaxNs = max(callWallMaxNs, callWallNs)
        self.handlerCPUNs += handlerCPUNs
    }

    /// The driver's last flipped frame, once per flip seen.
    mutating func observe(_ stats: PresentStats) {
        guard stats.flipped != lastFlipped else { return }
        lastFlipped = stats.flipped
        guard stats.lastBytes > 0 else { return }
        let i = PresentMeasurement.bucketLimits.firstIndex { stats.lastBytes < $0 } ?? 3
        bucketFrames[i] += 1
        bucketBytes[i] += stats.lastBytes
        bucketCopyNs[i] += stats.lastCopyGPUNs
    }

    /// The report: @start and @end are the driver's counters around the
    /// window of @seconds; @refreshHz the mode's rate.
    func lines(start: PresentStats, end: PresentStats, seconds: Double, refreshHz: Double) -> [String] {
        let flipped = end.flipped - start.flipped, received = end.received - start.received
        let replaced = end.replaced - start.replaced, bytes = end.bytesCopied - start.bytesCopied
        let f = Double(max(flipped, 1)), p = Double(max(presented, 1))
        var out: [String] = []
        out.append(String(format: "frames: %d captured (%d without damage), %d presented, %llu received by the driver, %llu flipped, %llu replaced before a flip; %.1f flips/s at %.3f Hz",
                          frames, idleFrames, presented, received, flipped, replaced,
                          Double(flipped) / max(seconds, 0.001), refreshHz))
        out.append(String(format: "agent per presented frame: PRESENT %.1f us CPU, %.1f us wall (max %.1f); whole frame handler %.1f us CPU",
                          Double(callCPUNs) / p / 1e3, Double(callWallNs) / p / 1e3,
                          Double(callWallMaxNs) / 1e3, Double(handlerCPUNs) / p / 1e3))
        out.append(String(format: "composition to the frame handler: %.2f ms on average, %.2f ms at most (%d frames)",
                          Double(lagNs) / Double(max(lagFrames, 1)) / 1e6, Double(lagMaxNs) / 1e6, lagFrames))
        out.append(String(format: "waiting for the compositor to finish the captured frame: %.1f us on average, %.1f us at most",
                          Double(lockWaitNs) / p / 1e3, Double(lockWaitMaxNs) / 1e3))
        let ff = Double(max(filterFrames, 1))
        out.append(String(format: "unchanged damage: %d of %d damaged frame(s) changed nothing; %.1f KB reported, %.1f KB kept per damaged frame (%.0f%% dropped); hashing %.1f us per frame",
                          unchangedFrames, filterFrames, Double(reportedBytes) / ff / 1e3, Double(keptBytes) / ff / 1e3,
                          reportedBytes > 0 ? 100 * (1 - Double(keptBytes) / Double(reportedBytes)) : 0,
                          Double(filterNs) / ff / 1e3))
        out.append(String(format: "scrolling: %d frame(s) with moves, %.1f KB moved per damaged frame; driver: %llu rows moved in VRAM, %llu move(s) copied from the surface instead; %.1f KB per flip copied in VRAM",
                          movedFrames, Double(movedBytes) / ff / 1e3, end.movedRows &- start.movedRows,
                          end.movesDropped &- start.movesDropped, Double(end.vramBytes &- start.vramBytes) / f / 1e3))
        out.append(String(format: "driver per flipped frame: %.1f KB copied from the surface (PCIe) in %.2f copy jobs, worker submit %.1f us CPU, GPU copy %.3f ms",
                          Double(bytes) / f / 1e3, Double(end.copyJobs - start.copyJobs) / f,
                          Double(end.copySubmitNs - start.copySubmitNs) / f / 1e3,
                          Double(end.copyGPUNs - start.copyGPUNs) / f / 1e6))
        out.append(String(format: "CPU accesses to the VRAM aperture: %.1f per flipped frame",
                          Double(end.apertureOps &- start.apertureOps) / f))
        out.append(String(format: "capture to scanout: %.2f ms on average, %.2f ms at most (since OUTPUT); %.2f frame(s) at %.3f Hz",
                          Double(end.latencyNs - start.latencyNs) / f / 1e6, Double(end.latencyMaxNs) / 1e6,
                          Double(end.latencyNs - start.latencyNs) / f / 1e9 * refreshHz, refreshHz))
        for i in 0..<4 where bucketFrames[i] > 0 {
            let mb = Double(bucketBytes[i]) / 1048576
            out.append(String(format: "copy %@: %d frame(s), %.1f KB each, %.3f ms each, %.2f ms per MiB",
                              PresentMeasurement.bucketNames[i], bucketFrames[i],
                              Double(bucketBytes[i]) / Double(bucketFrames[i]) / 1e3,
                              Double(bucketCopyNs[i]) / Double(bucketFrames[i]) / 1e6,
                              mb > 0 ? Double(bucketCopyNs[i]) / 1e6 / mb : 0))
        }
        return out
    }
}

/// Presented frames whose capture buffers must stay unreused until the
/// driver is done reading them. PRESENT only queues a frame (a mailbox of
/// one); the driver's worker later takes it and copies it (SDMA reads the
/// buffer) or a newer frame replaces it first (never read). A taken frame
/// is done at its flip: the flip waits for its copy, and frames flip in
/// the order they were taken. Which happened to a frame is known at the
/// next PRESENT: the replaced count went up when it was still in the
/// mailbox. The driver's counts alone do not say which frames are done:
/// a replacement counts the newest frame while an older one taken before
/// it may still be copying.
struct PresentedFrames<Item> {
    /// Taken frames: the flipped count at which each is done, oldest first.
    private(set) var taken: [(item: Item, flip: UInt64)] = []
    /// The newest frame: in the mailbox or taken, not known until the next PRESENT.
    private(set) var newest: Item?
    private var replaced: UInt64 = 0

    var count: Int { taken.count + (newest == nil ? 0 : 1) }

    /// @item was just queued; @received, @replaced and @flipped are the
    /// driver's counts PRESENT returned for it. Returns the items now free.
    mutating func presented(_ item: Item, received: UInt64, replaced nowReplaced: UInt64,
                            flipped: UInt64) -> [Item] {
        var free: [Item] = []
        if let previous = newest {
            if nowReplaced > replaced {
                free.append(previous)      // still in the mailbox: never read
            } else {
                // Taken: every frame before this one was taken or replaced,
                // so it was taken frame number (received - 1 - replaced).
                taken.append((previous, received - 1 - nowReplaced))
            }
        }
        newest = item
        replaced = nowReplaced
        free += retire(flipped: flipped)
        return free
    }

    /// Frames flipped by @flipped are done.
    mutating func retire(flipped: UInt64) -> [Item] {
        var free: [Item] = []
        while let first = taken.first, first.flip <= flipped {
            free.append(first.item)
            taken.removeFirst()
        }
        return free
    }

    mutating func removeAll() {
        taken.removeAll()
        newest = nil
        replaced = 0
    }
}

/// Damage that changes nothing on screen, dropped before PRESENT, and rows
/// that scrolled, sent as moves. Some windows are redrawn every frame with
/// the same pixels (menu bar status items, a cursor drawn by the hardware
/// rather than into the capture) and ScreenCaptureKit reports them as
/// damage; copied and flipped, an idle display would cost a frame's work
/// every refresh. Each row of the frame is cut into tiles of 64 pixels
/// with a 64-bit hash of each, of what the driver was last given. A dirty
/// rectangle's tiles are hashed again: tiles that did not change are
/// dropped; rows whose tiles equal another row's from before (a scroll,
/// found by the shift most rows agree on) become moves, which the driver
/// copies in VRAM; the rest are kept in whole tiles (a tile is never
/// recorded as given while part of it was not), consecutive changed rows
/// merged into one rectangle. Everything is compared with the hashes from
/// before the frame, so rectangles that overlap agree.
struct DamageFilter {
    typealias Rect = (x: UInt32, y: UInt32, w: UInt32, h: UInt32)
    /// @w x @h pixels at (@x, @y) are the previous frame's at (@x, @srcY).
    typealias Move = (x: UInt32, y: UInt32, w: UInt32, h: UInt32, srcY: UInt32)
    struct Damage {
        var rects: [Rect] = []
        var moves: [Move] = []
        var isEmpty: Bool { rects.isEmpty && moves.isEmpty }
    }
    static let tile = 64
    /// PRESENT's limits: moves, and the request (16 bytes, 16 per rectangle, 24 per move).
    static let movesMax = 32, requestMax = 4096
    /// Shorter runs of scrolled rows are damage: a move per row or two is not worth it.
    static let moveRowsMin = 4
    let width: Int, height: Int, columns: Int
    /// Off: no moves, only unchanged tiles dropped.
    var detectScroll = true
    /// Per row, per tile: the hash of what the driver was given; 0 when
    /// not known (hashes are odd).
    private var hashes: [UInt64]
    /// Scratch.
    private var fresh: [UInt64] = [], newRow: [UInt64] = [], oldRow: [UInt64] = []

    init(width: Int, height: Int) {
        self.width = width
        self.height = height
        columns = (width + DamageFilter.tile - 1) / DamageFilter.tile
        hashes = [UInt64](repeating: 0, count: columns * height)
    }

    /// Nothing known: the next damage is kept whole.
    mutating func reset() {
        for i in hashes.indices { hashes[i] = 0 }
    }

    /// The damage of @rects that changed anything, given the frame at
    /// @base (BGRA, @pitch bytes per row, readable while this runs). When
    /// it does not fit PRESENT, the whole frame.
    mutating func filter(_ rects: [Rect], base: UnsafeRawPointer, pitch: Int) -> Damage {
        var out = Damage()
        var updates: [(at: Int, rows: Int, c0: Int, n: Int, hashes: [UInt64])] = []
        let t = DamageFilter.tile
        for r in rects {
            let x0 = Int(r.x), y0 = Int(r.y)
            let x1 = min(x0 + Int(r.w), width), y1 = min(y0 + Int(r.h), height)
            guard x0 < x1, y0 < y1 else { continue }
            let c0 = x0 / t, c1 = (x1 - 1) / t, n = c1 - c0 + 1, rows = y1 - y0
            fresh.removeAll(keepingCapacity: true)
            for y in y0..<y1 {
                let row = base + y * pitch
                for c in c0...c1 {
                    let px = c * t, count = min(t, width - px)
                    fresh.append(DamageFilter.hash(row + px * 4, bytes: count * 4))
                }
            }
            // Row signatures over the rectangle's tiles, new and as given (0: not known).
            newRow.removeAll(keepingCapacity: true)
            oldRow.removeAll(keepingCapacity: true)
            for i in 0..<rows {
                newRow.append(DamageFilter.fold(fresh, from: i * n, count: n))
                oldRow.append(DamageFilter.fold(hashes, from: (y0 + i) * columns + c0, count: n))
            }
            let shift = detectScroll && out.moves.count < DamageFilter.movesMax ? scrollShift(rows) : 0
            let px0 = c0 * t, pxw = min((c1 + 1) * t, width) - px0
            var runStart = -1, runMin = 0, runMax = 0, moveStart = -1
            func close(_ end: Int) {
                guard runStart >= 0 else { return }
                let px = runMin * t
                out.rects.append((UInt32(px), UInt32(runStart), UInt32(min((runMax + 1) * t, width) - px),
                                  UInt32(end - runStart)))
                runStart = -1
            }
            func closeMove(_ end: Int) {
                guard moveStart >= 0 else { return }
                let count = end - moveStart
                if count >= DamageFilter.moveRowsMin && out.moves.count < DamageFilter.movesMax {
                    out.moves.append((UInt32(px0), UInt32(moveStart), UInt32(pxw), UInt32(count),
                                      UInt32(moveStart + shift)))
                } else {
                    // Too short to move: damage, its changed tiles.
                    for y in moveStart..<end { tileRow(y, y0: y0, c0: c0, n: n) }
                }
                moveStart = -1
            }
            func tileRow(_ y: Int, y0: Int, c0: Int, n: Int) {
                var lo = Int.max, hi = -1
                for i in 0..<n where hashes[y * columns + c0 + i] != fresh[(y - y0) * n + i] {
                    lo = min(lo, c0 + i)
                    hi = c0 + i
                }
                if hi < 0 {
                    close(y)
                } else if runStart < 0 {
                    runStart = y; runMin = lo; runMax = hi
                } else if runStart >= 0 {
                    runMin = min(runMin, lo); runMax = max(runMax, hi)
                }
            }
            for y in y0..<y1 {
                let i = y - y0, src = i + shift
                if shift != 0 && newRow[i] != oldRow[i] && src >= 0 && src < rows && oldRow[src] != 0 &&
                    oldRow[src] == newRow[i] {
                    close(y)
                    if moveStart < 0 { moveStart = y }
                    continue
                }
                closeMove(y)
                tileRow(y, y0: y0, c0: c0, n: n)
            }
            closeMove(y1)
            close(y1)
            updates.append((y0 * columns + c0, rows, c0, n, fresh))
        }
        for u in updates {
            for i in 0..<u.rows {
                for k in 0..<u.n { hashes[u.at + i * columns + k] = u.hashes[i * u.n + k] }
            }
        }
        // Overlapping rectangles report the tiles they share each: once is enough.
        var seen = Set<[UInt32]>()
        out.rects = out.rects.filter { seen.insert([$0.x, $0.y, $0.w, $0.h]).inserted }
        seen.removeAll()
        out.moves = out.moves.filter { seen.insert([$0.x, $0.y, $0.w, $0.h, $0.srcY]).inserted }
        if 16 + out.rects.count * 16 + out.moves.count * 24 > DamageFilter.requestMax || out.rects.count > 255 {
            return Damage(rects: [(0, 0, UInt32(width), UInt32(height))], moves: [])
        }
        return out
    }

    /// The vertical shift most changed rows of the rectangle agree on (a
    /// row's new signature found at row + shift before), counted only
    /// over rows whose old signature is unique (blank rows say nothing);
    /// 0 when fewer than moveRowsMin agree.
    private func scrollShift(_ rows: Int) -> Int {
        guard rows >= 2 * DamageFilter.moveRowsMin else { return 0 }
        var at: [UInt64: Int] = [:], repeated = Set<UInt64>()
        at.reserveCapacity(rows)
        for i in 0..<rows where oldRow[i] != 0 {
            if at.updateValue(i, forKey: oldRow[i]) != nil { repeated.insert(oldRow[i]) }
        }
        var votes: [Int: Int] = [:]
        for i in 0..<rows where newRow[i] != oldRow[i] {
            if let src = at[newRow[i]], !repeated.contains(newRow[i]), src != i { votes[src - i, default: 0] += 1 }
        }
        guard let best = votes.max(by: { $0.value < $1.value || ($0.value == $1.value && abs($0.key) > abs($1.key)) }),
              best.value >= DamageFilter.moveRowsMin else { return 0 }
        return best.key
    }

    /// One signature for @count tile hashes from @from; 0 if one is not known.
    private static func fold(_ h: [UInt64], from: Int, count: Int) -> UInt64 {
        var s: UInt64 = 0x2d358dccaa6c78a5
        for k in 0..<count {
            let v = h[from + k]
            if v == 0 { return 0 }
            s = mum(s ^ v, 0x8bb84b93962eacc9)
        }
        return s | 1
    }

    @inline(__always) private static func mum(_ a: UInt64, _ b: UInt64) -> UInt64 {
        let r = a.multipliedFullWidth(by: b)
        return r.high ^ r.low
    }

    /// A 64-bit hash of @bytes (a multiple of 4) at @p, odd (0 is "not
    /// known"): wyhash's multiply-fold over two lanes of 16 bytes.
    static func hash(_ p: UnsafeRawPointer, bytes: Int) -> UInt64 {
        let k0: UInt64 = 0xa0761d6478bd642f, k1: UInt64 = 0xe7037ed1a0b428db
        let k2: UInt64 = 0x8ebc6af09c88c6e3, k3: UInt64 = 0x589965cc75374cc3
        var a = k0 ^ UInt64(bytes), b = k1
        var i = 0
        while i + 32 <= bytes {
            a = mum(p.loadUnaligned(fromByteOffset: i, as: UInt64.self) ^ k1,
                    p.loadUnaligned(fromByteOffset: i + 8, as: UInt64.self) ^ a)
            b = mum(p.loadUnaligned(fromByteOffset: i + 16, as: UInt64.self) ^ k2,
                    p.loadUnaligned(fromByteOffset: i + 24, as: UInt64.self) ^ b)
            i += 32
        }
        while i + 8 <= bytes {
            a = mum(p.loadUnaligned(fromByteOffset: i, as: UInt64.self) ^ k3, a ^ k1)
            i += 8
        }
        if i < bytes {
            a = mum(UInt64(p.loadUnaligned(fromByteOffset: i, as: UInt32.self)) ^ k2, a ^ k3)
        }
        return mum(a ^ k3, b ^ k0 ^ UInt64(bytes)) | 1
    }
}

/// A frame's damage as PRESENT takes it: whole pixels inside the surface,
/// at most 255 rectangles (more become the whole frame).
func presentRects(_ dirty: [CGRect], width: Int, height: Int) -> [(x: UInt32, y: UInt32, w: UInt32, h: UInt32)] {
    let bounds = CGRect(x: 0, y: 0, width: width, height: height)
    var out: [(x: UInt32, y: UInt32, w: UInt32, h: UInt32)] = []
    for rect in dirty {
        let r = rect.standardized.intersection(bounds)
        if r.isNull || r.isEmpty { continue }
        let x0 = Int(r.minX.rounded(.down)), y0 = Int(r.minY.rounded(.down))
        let x1 = Int(r.maxX.rounded(.up)), y1 = Int(r.maxY.rounded(.up))
        out.append((UInt32(x0), UInt32(y0), UInt32(x1 - x0), UInt32(y1 - y0)))
    }
    if out.count > 255 { return [(0, 0, UInt32(width), UInt32(height))] }
    return out
}

// ----------------------------------------------------------------
// MARK: - the display control model (daemon <-> menu bar)
// ----------------------------------------------------------------

/// A monitor's identity across connectors, replugs and GPUs: its EDID
/// manufacturer, product code and serial (the serial string descriptor
/// when the binary serial is 0). nil without a readable EDID.
func monitorKey(edid: Data?) -> String? {
    guard let edid, let summary = EDIDSummary(edid) else { return nil }
    let serial = summary.serial != 0 ? String(summary.serial) : (summary.serialText ?? "0")
    return String(format: "%@-%04X-%@", summary.vendor, summary.product, serial)
}

/// The user's per-monitor choices (the menu bar's toggles): a monitor is
/// mirrored unless turned off. Persisted as JSON, keyed by monitorKey.
struct DisplayPrefs: Codable, Equatable {
    var off: [String] = []

    func isOn(_ key: String) -> Bool { !off.contains(key) }
    mutating func set(_ key: String, on: Bool) {
        off.removeAll { $0 == key }
        if !on { off.append(key); off.sort() }
    }

    static func load(from url: URL) -> DisplayPrefs {
        guard let data = try? Data(contentsOf: url) else { return DisplayPrefs() }
        return (try? JSONDecoder().decode(DisplayPrefs.self, from: data)) ?? DisplayPrefs()
    }
    func save(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
}

/// What the daemon reports for the menu bar.
struct DisplayStatus: Codable, Equatable {
    enum State: String, Codable { case off, starting, mirroring, error }
    struct Monitor: Codable, Equatable {
        var key: String
        var connector: String
        var name: String
        var on: Bool
        var state: State
        var mode: String?          // "2560x1440 @ 59.950 Hz" while mirroring
        var error: String?         // the last error line
    }
    var daemon: Int32 = 0          // the daemon's pid, 0 when it is not running
    var driverAttached = false
    var monitors: [Monitor] = []

    /// The menu bar icon's state: an error on any monitor, else whether
    /// any is mirrored.
    var summary: State {
        if monitors.contains(where: { $0.state == .error }) { return .error }
        if monitors.contains(where: { $0.state == .mirroring }) { return .mirroring }
        if monitors.contains(where: { $0.state == .starting }) { return .starting }
        return .off
    }

    static func load(from url: URL) -> DisplayStatus? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(DisplayStatus.self, from: data)
    }
    func save(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
}

/// What a mirroring process's output line says about its monitor.
enum AgentLine: Equatable {
    case mirroring(String)
    case failed(String)
    case other
}
func parseAgentLine(_ line: String) -> AgentLine {
    if let range = line.range(of: " lit at ") {
        return .mirroring(String(line[range.upperBound...]).trimmingCharacters(in: .whitespaces))
    }
    if let range = line.range(of: "FAILED: ") { return .failed(String(line[range.upperBound...])) }
    return .other
}

/// The connected monitors to mirror: those with an identity the user has
/// not turned off. (A monitor without a readable EDID cannot be told apart
/// across replugs; it is mirrored, under its connector's name.)
func connectorsToMirror(connected: [(connector: String, key: String)], prefs: DisplayPrefs) -> [String] {
    connected.filter { prefs.isOn($0.key) }.map { $0.connector }
}

/// Where the daemon and the menu bar meet: the choices, the status, and the
/// Darwin notifications each posts when it changed its file.
enum DisplayControl {
    static var directory: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/MacLinuxGPU")
    }
    static var prefsURL: URL { directory.appendingPathComponent("displays.json") }
    static var statusURL: URL { directory.appendingPathComponent("display-status.json") }
    static let prefsChanged = "com.geramyloveless.maclinuxgpu.display-prefs"
    static let statusChanged = "com.geramyloveless.maclinuxgpu.display-status"
}

/// Durations of one stage of a latency (ns), summarized as percentiles.
struct LatencySeries {
    private(set) var values: [UInt64] = []
    mutating func add(_ ns: UInt64) { values.append(ns) }
    var count: Int { values.count }
    /// The value at @fraction (0...1) of the sorted series, nearest rank.
    func percentile(_ fraction: Double) -> UInt64 {
        guard !values.isEmpty else { return 0 }
        let sorted = values.sorted()
        let rank = Int((fraction * Double(sorted.count)).rounded(.up)) - 1
        return sorted[min(max(rank, 0), sorted.count - 1)]
    }
    /// "p50 a ms, p90 b ms, max c ms (n)".
    var summary: String {
        String(format: "p50 %.2f ms, p90 %.2f ms, max %.2f ms (%d)", Double(percentile(0.5)) / 1e6,
               Double(percentile(0.9)) / 1e6, Double(values.max() ?? 0) / 1e6, values.count)
    }
}

/// The type workload's marks: four glyph-sized squares, square i white
/// when bit i of the change number is set, else black (black and white
/// survive the display's colour conversion, grey levels do not). The four
/// captured pixels' brightness gives seq % 16 back.
func typeMarkBit(_ seq: Int, _ bit: Int) -> Bool { (seq >> bit) & 1 == 1 }
func typeMarkIndex(brightness: [Int]) -> Int {
    brightness.prefix(4).enumerated().reduce(0) { $0 | ($1.element >= 128 ? 1 << $1.offset : 0) }
}
