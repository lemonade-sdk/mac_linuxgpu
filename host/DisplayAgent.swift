//
//  DisplayAgent.swift — the display agent's model: what the driver reports
//  about the GPU's outputs (selector 84, dext/sources/session_state.h), each
//  connected monitor's EDID, and the macOS virtual display that would stand
//  for it (docs/macos-displays.md). Foundation only: no IOKit and no
//  CoreGraphics, so it builds and is tested on its own
//  (scripts/test-display-agent.sh); the host app's display-agent command
//  drives it over the observer client.
//

import Foundation

// struct rt_display_report / rt_display_modes (linuxu/headers/rt/display.h),
// version 1.
let kDisplayReportBytes = 72 + 8 * 80
let kDisplayModesBytes = 24 + 56 * 16

enum DisplayOp: UInt64 { case probe = 0, show = 1, off = 2, status = 3, modes = 4 }
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
