// The display agent's model (host/DisplayAgent.swift) against synthetic
// driver replies and EDIDs: report and mode decoding, the virtual-display
// plan for a monitor, and the changes a probe makes. No IOKit, no
// CoreGraphics; compiled with DisplayAgent.swift by
// scripts/test-display-agent.sh.
import Foundation

func check(_ condition: Bool, _ message: String = "", file: String = #file, line: Int = #line) {
    if !condition {
        FileHandle.standardError.write("\(file):\(line): check failed \(message)\n".data(using: .utf8)!)
        exit(1)
    }
}

func put32(_ bytes: inout [UInt8], _ at: Int, _ value: UInt32) {
    for i in 0..<4 { bytes[at + i] = UInt8((value >> (8 * UInt32(i))) & 0xff) }
}

/// EDID 1.4 base block: "LNX" product 0x0001, 60x34 cm, sRGB primaries,
/// 1920x1080@60 preferred, a monitor name and (optionally) a serial string.
func edid(name: String = "TEST PANEL", serial: UInt32 = 4242, serialText: String? = nil) -> Data {
    var b = [UInt8](repeating: 0, count: 128)
    b[0..<8] = [0, 255, 255, 255, 255, 255, 255, 0]
    b[8] = 0x31; b[9] = 0xd8
    b[10] = 1
    put32(&b, 12, serial)
    b[16] = 1; b[17] = 36; b[18] = 1; b[19] = 4; b[20] = 0xa5; b[21] = 60; b[22] = 34
    b[25..<35] = [0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54]
    b[54..<72] = [0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00,
                  0x58, 0x54, 0x21, 0x00, 0x00, 0x1e]
    func text(_ at: Int, _ tag: UInt8, _ s: String) {
        b[at + 3] = tag
        var t = Array(s.utf8.prefix(13))
        if t.count < 13 { t.append(0x0a) }
        while t.count < 13 { t.append(0x20) }
        b[(at + 5)..<(at + 18)] = t[...]
    }
    text(72, 0xfc, name)
    if let serialText { text(90, 0xff, serialText) } else { b[90 + 3] = 0x10 }
    b[108 + 3] = 0x10
    b[127] = UInt8((256 - Int(b[0..<127].reduce(UInt8(0), &+))) & 0xff)
    return Data(b)
}

/// struct rt_display_modes.
func modesBlob(status: UInt32 = 1, mm: (Int, Int) = (600, 340),
               _ list: [(Int, Int, Int, Int, UInt32)]) -> Data {
    var b = [UInt8](repeating: 0, count: kDisplayModesBytes)
    put32(&b, 0, 1); put32(&b, 4, status); put32(&b, 8, UInt32(list.count)); put32(&b, 12, UInt32(list.count))
    put32(&b, 16, UInt32(mm.0)); put32(&b, 20, UInt32(mm.1))
    for (i, m) in list.enumerated() {
        let at = 24 + i * 16
        b[at] = UInt8(m.0 & 0xff); b[at + 1] = UInt8(m.0 >> 8)
        b[at + 2] = UInt8(m.1 & 0xff); b[at + 3] = UInt8(m.1 >> 8)
        put32(&b, at + 4, UInt32(m.2)); put32(&b, at + 8, UInt32(m.3)); put32(&b, at + 12, m.4)
    }
    return Data(b)
}

let native = (1920, 1080, 60000, 148500, UInt32(1))
let monitorModes: [(Int, Int, Int, Int, UInt32)] = [
    (1920, 1080, 59940, 148352, 0), native, (1920, 1080, 60000, 148500, 0),   // a duplicate
    (1920, 1080, 120000, 297000, 2),                                       // interlaced
    (1280, 720, 60000, 74250, 0), (640, 480, 59940, 25175, 0),
]

// ---- decoding ----
guard let modes = DisplayModes(modesBlob(monitorModes)) else { check(false, "modes"); exit(1) }
check(modes.modes.count == 6 && modes.widthMm == 600 && modes.status == 1)
check(modes.modes[1].preferred && modes.modes[3].interlaced && modes.modes[0].refresh == 59.94)
check(DisplayModes(Data(count: 10)) == nil)

var reportBytes = [UInt8](repeating: 0, count: kDisplayReportBytes)
put32(&reportBytes, 0, 1); put32(&reportBytes, 4, 1); put32(&reportBytes, 68, 9)
reportBytes[72..<76] = [0x44, 0x50, 0x2d, 0x31]                 // "DP-1"
put32(&reportBytes, 72 + 36, 1)                                 // connected
guard let report = DisplayReport(Data(reportBytes)) else { check(false, "report"); exit(1) }
check(report.hotplugEpoch == 9 && report.connectors.count == 1 && report.connectors[0].name == "DP-1" &&
      report.connectors[0].connected)

guard let info = EDIDSummary(edid()) else { check(false, "edid"); exit(1) }
check(info.manufacturerID == 0x31d8 && info.vendor == "LNX" && info.name == "TEST PANEL")
// sRGB primaries as EDIDs encode them (10-bit): red 0.640/0.330, white 0.3125/0.329.
check(abs(info.red.x - 0.640) < 0.002 && abs(info.red.y - 0.330) < 0.002)
check(abs(info.green.x - 0.300) < 0.002 && abs(info.blue.y - 0.060) < 0.002)
check(abs(info.white.x - 0.3125) < 0.002 && abs(info.white.y - 0.329) < 0.002)

// ---- the plan ----
guard case .success(let plan) = planVirtualDisplay(connector: "DP-1", modes: modes, edid: edid()) else {
    check(false, "plan"); exit(1)
}
check(plan.name == "TEST PANEL" && plan.vendorID == 0x31d8 && plan.productID == 1 && plan.serialNum == 4242)
check(plan.sizeInMillimeters == (600, 340) && plan.maxPixelsWide == 1920 && plan.maxPixelsHigh == 1080)
check(plan.hiDPI == 0)
check(plan.modes.map { "\($0.width)x\($0.height)@\($0.refreshRate)" } ==
      ["1920x1080@60.0", "1920x1080@59.94", "1280x720@60.0", "640x480@59.94"],
      "\(plan.modes)")                                           // preferred first, no duplicate or interlaced
check(plan.lines.contains { $0.contains("vendorID 0x31d8  productID 0x0001  serialNum 4242") })

// Serial from the EDID's serial string when the number is 0; the EDID's
// own size when the driver knows none; the PNP ID and product as the name
// when the EDID has none.
if case .success(let p) = planVirtualDisplay(connector: "DP-2",
                                              modes: DisplayModes(modesBlob(mm: (0, 0), [native]))!,
                                              edid: edid(name: "", serial: 0, serialText: "SN12345")) {
    check(p.serialNum != 0 && p.sizeInMillimeters == (600, 340) && p.name == "LNX 0001", "\(p.name)")
} else { check(false, "plan 2") }

// What cannot become a display.
check(planVirtualDisplay(connector: "DP-3", modes: DisplayModes(modesBlob(status: 2, []))!, edid: edid())
      == .failure(.notConnected))
check(planVirtualDisplay(connector: "DP-1", modes: modes, edid: nil) == .failure(.noEDID))
var corrupt = [UInt8](edid()); corrupt[20] ^= 1
check(planVirtualDisplay(connector: "DP-1", modes: modes, edid: Data(corrupt)) == .failure(.noEDID))
check(planVirtualDisplay(connector: "DP-1", modes: DisplayModes(modesBlob([(1920, 1080, 120000, 297000, 3)]))!,
                         edid: edid()) == .failure(.noProgressiveMode))

// ---- the agent's changes ----
var state = DisplayAgentState()
check(state.needsProbe(epoch: 1) && !state.needsProbe(epoch: 1) && state.needsProbe(epoch: 2))
check(state.apply([plan]) == [.add(plan)])
check(state.apply([plan]) == [.unchanged("DP-1")])
let fewer = planVirtualDisplay(connector: "DP-1", modes: DisplayModes(modesBlob([native]))!, edid: edid())
guard case .success(let changed) = fewer else { check(false); exit(1) }
let second = planVirtualDisplay(connector: "DP-2", modes: modes, edid: edid(name: "OTHER"))
guard case .success(let other) = second else { check(false); exit(1) }
check(state.apply([changed, other]) == [.update(changed), .add(other)])
check(state.apply([other]) == [.remove("DP-1"), .unchanged("DP-2")])
check(state.apply([]) == [.remove("DP-2")] && state.displays.isEmpty)
check(String(describing: DisplayAgentState.Change.add(plan)) == "add DP-1 (\"TEST PANEL\")")

print("PASS display agent: report/modes decoding, EDID identity and primaries, virtual-display plans, refusals, add/update/remove")
