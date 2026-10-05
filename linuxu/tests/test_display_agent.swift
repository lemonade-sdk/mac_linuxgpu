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

// ---- frames ----
// rt_surface_pattern, as the driver computes it (C: (uint32_t)(dword * 2654435761u) ^ seed * 0x85ebca77u ^ 0x5a5a0000u).
check(surfacePattern(seed: 0, dword: 0) == 0x5a5a0000)
check(surfacePattern(seed: 1, dword: 1) == (2654435761 ^ 0x85ebca77 ^ 0x5a5a0000))
check(surfacePattern(seed: 3, dword: 0x1_0000_0001) == UInt32(truncatingIfNeeded: UInt64(0x1_0000_0001) &* 2654435761) ^ (3 &* 0x85ebca77) ^ 0x5a5a0000)
var verify = [UInt8](repeating: 0, count: SurfaceVerifyResult.bytes)
put32(&verify, 0, 1); put32(&verify, 4, 64); put32(&verify, 8, 256); put32(&verify, 12, 3); put32(&verify, 20, 1)
put32(&verify, 24, 4096); put32(&verify, 56, 0xdead); put32(&verify, 60, 0xbeef)
guard let v = SurfaceVerifyResult(Data(verify)) else { check(false); exit(1) }
check(v.dwords == 4096 && v.gpuMismatches == 3 && v.cpuChecked && v.firstGPUMismatch == 4096 &&
      v.gpuValue == 0xdead && v.expectedValue == 0xbeef)
func put64(_ b: inout [UInt8], _ at: Int, _ v: UInt64) {
    put32(&b, at, UInt32(truncatingIfNeeded: v)); put32(&b, at + 4, UInt32(truncatingIfNeeded: v >> 32))
}
func presentStats(flipped: UInt64, bytes: UInt64, lastBytes: UInt64, lastCopy: UInt64, latency: UInt64,
                  vram: UInt64 = 0, moved: UInt64 = 0, dropped: UInt64 = 0) -> PresentStats? {
    var b = [UInt8](repeating: 0, count: PresentStats.bytes)
    put32(&b, 0, 3); put32(&b, 4, 2)
    put64(&b, 120, vram); put64(&b, 128, moved); put64(&b, 136, dropped)
    put64(&b, 8, flipped + 1); put64(&b, 16, flipped); put64(&b, 24, 1); put64(&b, 32, flipped * 2)
    put64(&b, 40, bytes); put64(&b, 48, flipped * 30_000); put64(&b, 56, flipped * 500_000)
    put64(&b, 64, latency); put64(&b, 72, 20_000_000)
    put64(&b, 80, lastBytes); put64(&b, 88, lastCopy); put64(&b, 96, 9_000_000)
    put32(&b, 104, UInt32(bitPattern: -19)); put32(&b, 108, 3)
    return PresentStats(Data(b))
}
guard let ps = presentStats(flipped: 9, bytes: 7680, lastBytes: 4096, lastCopy: 70_000, latency: 90_000_000)
else { check(false); exit(1) }
check(ps.engines == 2 && ps.received == 10 && ps.flipped == 9 && ps.replaced == 1 && ps.copyJobs == 18 &&
      ps.bytesCopied == 7680 && ps.copyGPUNs == 4_500_000 && ps.latencyMaxNs == 20_000_000 &&
      ps.lastBytes == 4096 && ps.lastCopyGPUNs == 70_000 && ps.lastLatencyNs == 9_000_000 &&
      ps.error == -19 && ps.fullFrames == 3)
var v1 = [UInt8](repeating: 0, count: PresentStats.bytes); put32(&v1, 0, 1)
check(PresentStats(Data(v1)) == nil && PresentStats(Data(count: 56)) == nil, "version 1 and short replies refused")
// Version 2 (a driver before 240): 120 bytes, no VRAM counters. Version 3 must be whole.
var v2 = [UInt8](repeating: 0, count: 120); put32(&v2, 0, 2); put64(&v2, 40, 77)
check(PresentStats(Data(v2)).map { $0.bytesCopied == 77 && $0.vramBytes == 0 && $0.movedRows == 0 } == true)
var v3short = [UInt8](repeating: 0, count: 120); put32(&v3short, 0, 3)
check(PresentStats(Data(v3short)) == nil, "a short version 3 reply refused")
// The measurement: one flip counted once, copy time by damage size.
var m = PresentMeasurement()
m.frames = 4; m.idleFrames = 1
m.present(callCPUNs: 20_000, callWallNs: 40_000, handlerCPUNs: 60_000)
m.present(callCPUNs: 30_000, callWallNs: 80_000, handlerCPUNs: 90_000, lockWaitNs: 1_200_000)
m.delivered(lagNs: 2_000_000); m.delivered(lagNs: 4_000_000)
m.filtered(reported: [(0, 0, 100, 10)], kept: DamageFilter.Damage(rects: [(0, 0, 50, 10)], moves: [(0, 20, 100, 30, 25)]), ns: 30_000)
m.filtered(reported: [(0, 0, 100, 10)], kept: DamageFilter.Damage(), ns: 10_000)
m.observe(ps); m.observe(ps)
let big = presentStats(flipped: 10, bytes: 7680 + 8_294_400, lastBytes: 8_294_400, lastCopy: 2_000_000, latency: 100_000_000,
                       vram: 51_200, moved: 300, dropped: 2)!
m.observe(big)
check(m.bucketFrames == [1, 0, 0, 1] && m.bucketBytes[3] == 8_294_400 && m.callWallMaxNs == 80_000)
let measured = m.lines(start: ps, end: big, seconds: 1, refreshHz: 60)
check(measured.count == 11 && measured[1].contains("PRESENT 25.0 us CPU, 60.0 us wall (max 80.0)") &&
      measured[2].contains("composition to the frame handler: 3.00 ms on average, 4.00 ms at most (2 frames)") &&
      measured[3].contains("600.0 us on average, 1200.0 us at most") &&
      measured[4].contains("1 of 2 damaged frame(s) changed nothing; 4.0 KB reported, 1.0 KB kept per damaged frame (75% dropped); hashing 20.0 us") &&
      measured[5].contains("1 frame(s) with moves, 6.0 KB moved per damaged frame; driver: 300 rows moved in VRAM, 2 move(s) copied from the surface instead; 51.2 KB per flip copied in VRAM") &&
      measured[6].contains("8294.4 KB copied from the surface") && measured[8].contains("10.00 ms on average") &&
      measured[10].hasPrefix("copy >=4M: 1 frame(s)"), measured.joined(separator: "\n"))
// Damage: whole pixels, clipped, empty dropped, too many become the frame.
let r = presentRects([CGRect(x: 10.5, y: 20.2, width: 5, height: 5), CGRect(x: -10, y: -10, width: 20, height: 20),
                      CGRect(x: 3000, y: 0, width: 5, height: 5)], width: 2560, height: 1440)
check(r.count == 2 && r[0] == (10, 20, 6, 6) && r[1] == (0, 0, 10, 10), "\(r)")
check(presentRects((0..<300).map { CGRect(x: $0, y: 0, width: 1, height: 1) }, width: 640, height: 480)
      .elementsEqual([(0, 0, 640, 480)], by: ==))

print("PASS display agent: report/modes decoding, EDID identity and primaries, virtual-display plans, refusals, add/update/remove, frame replies and damage")

// ---- the display control model: per-monitor persistence, status, lines ----
do {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("mlg-display-control-\(getpid())")
    defer { try? FileManager.default.removeItem(at: dir) }
    let prefsURL = dir.appendingPathComponent("displays.json")
    // No file: every monitor is on.
    var prefs = DisplayPrefs.load(from: prefsURL)
    check(prefs.isOn("DEL-40DD-1096046924"))
    prefs.set("DEL-40DD-1096046924", on: false)
    prefs.set("DEL-40DD-1096046924", on: false)
    try! prefs.save(to: prefsURL)
    // Reloaded (a reboot, a replug): the choice holds, once.
    var reloaded = DisplayPrefs.load(from: prefsURL)
    check(reloaded == prefs && reloaded.off == ["DEL-40DD-1096046924"] && !reloaded.isOn("DEL-40DD-1096046924"))
    check(reloaded.isOn("SAM-0F12-77"), "other monitors stay on")
    // Keyed by identity, not connector: the same monitor on another
    // connector stays off; another monitor on its old connector is on.
    let connected = [(connector: "DP-2", key: "DEL-40DD-1096046924"), (connector: "DP-4", key: "SAM-0F12-77")]
    check(connectorsToMirror(connected: connected, prefs: reloaded) == ["DP-4"])
    reloaded.set("DEL-40DD-1096046924", on: true)
    check(connectorsToMirror(connected: connected, prefs: reloaded) == ["DP-2", "DP-4"])
    // A damaged file reads as defaults rather than failing the daemon.
    try! Data("not json".utf8).write(to: prefsURL)
    check(DisplayPrefs.load(from: prefsURL) == DisplayPrefs())

    // The key from an EDID: vendor, product, serial.
    let key = monitorKey(edid: edid(serial: 1096046924))
    check(key == "LNX-0001-1096046924", key ?? "nil")
    check(monitorKey(edid: edid(name: "", serial: 0, serialText: "SN12345")) == "LNX-0001-SN12345")
    check(monitorKey(edid: Data(count: 128)) == nil && monitorKey(edid: nil) == nil)

    // Status: written by the daemon, read by the menu bar.
    let statusURL = dir.appendingPathComponent("display-status.json")
    var status = DisplayStatus(daemon: 42, driverAttached: true, monitors: [
        .init(key: "DEL-40DD-1096046924", connector: "DP-4", name: "DELL UP2716D", on: true,
              state: .mirroring, mode: "2560x1440 @ 59.950 Hz", error: nil)])
    try! status.save(to: statusURL)
    check(DisplayStatus.load(from: statusURL) == status && status.summary == .mirroring)
    status.monitors.append(.init(key: "SAM-0F12-77", connector: "DP-2", name: "SAMSUNG", on: true,
                                 state: .error, mode: nil, error: "OUTPUT: busy"))
    check(status.summary == .error)
    check(DisplayStatus().summary == .off)

    // A mirroring process's lines.
    check(parseAgentLine("display-agent: DP-4 lit at 2560x1440 @ 59.950 Hz") == .mirroring("2560x1440 @ 59.950 Hz"))
    check(parseAgentLine("display-agent: FAILED: PRESENT: Linux errno 62") == .failed("PRESENT: Linux errno 62"))
    check(parseAgentLine("display-agent: capture surface 1 imported as handle 2") == .other)
}
print("PASS display control: per-monitor choices persist by EDID identity, status round trip, agent lines")

// Presented frames' buffers: free once the driver is done reading them.
do {
    var frames = PresentedFrames<String>()
    // A is queued; the worker takes it and starts copying.
    check(frames.presented("A", received: 1, replaced: 0, flipped: 0) == [])
    // B is queued (A was taken: the replaced count did not move).
    check(frames.presented("B", received: 2, replaced: 0, flipped: 0) == [])
    // C replaces B in the mailbox. A is still being copied: the driver's
    // counts (flipped + replaced = 1 = A's received) do not mean A is done.
    check(frames.presented("C", received: 3, replaced: 1, flipped: 0) == ["B"], "B was never read")
    check(frames.count == 2, "A (copying) and C (newest) held")
    // The worker took C; A flipped.
    check(frames.presented("D", received: 4, replaced: 1, flipped: 1) == ["A"])
    // C is the second frame taken: done at the second flip, not before.
    check(frames.retire(flipped: 1) == [])
    check(frames.retire(flipped: 2) == ["C"])
    check(frames.count == 1, "D stays until the next PRESENT says what happened to it")
    // A run of replacements frees each frame at once.
    check(frames.presented("E", received: 5, replaced: 2, flipped: 2) == ["D"])
    check(frames.presented("F", received: 6, replaced: 3, flipped: 2) == ["E"])
    frames.removeAll()
    check(frames.count == 0)
    // Counts from an earlier run of the output (the first frame classifies nothing).
    check(frames.presented("G", received: 101, replaced: 40, flipped: 59) == [])
    check(frames.presented("H", received: 102, replaced: 40, flipped: 59) == [])
    // G is frame 101; 40 before H were replaced, so G was the 61st taken.
    check(frames.retire(flipped: 60) == [])
    check(frames.retire(flipped: 61) == ["G"])
}
print("PASS presented frames: a buffer is held until its frame was replaced or flipped")

// Damage that changed nothing is dropped; what changed is kept in whole tiles.
do {
    let w = 300, h = 200, pitch = 1280   // 5 tiles a row, the last 44 pixels
    var px = [UInt32](repeating: 0x00336699, count: pitch / 4 * h)
    var f = DamageFilter(width: w, height: h)
    func run(_ rects: [DamageFilter.Rect]) -> [DamageFilter.Rect] {
        let d = px.withUnsafeBytes { f.filter(rects, base: $0.baseAddress!, pitch: pitch) }
        check(d.moves.isEmpty, "no scroll here: \(d.moves)")
        return d.rects
    }
    func same(_ a: [DamageFilter.Rect], _ b: [DamageFilter.Rect]) -> Bool { a.elementsEqual(b, by: ==) }
    // Nothing known yet: the whole rectangle, out to whole tiles.
    let first = run([(100, 40, 100, 30)])
    check(same(first, [(64, 40, 192, 30)]), "\(first)")
    // The same pixels again: nothing.
    check(run([(100, 40, 100, 30)]).isEmpty)
    // The rest of the frame, once, so later checks see only what they change.
    check(same(run([(0, 0, 300, 200)]), [(0, 0, 300, 200)]), "every row had a tile not known")
    // One pixel: its tile on its row.
    px[50 * pitch / 4 + 130] = 0x00ff0000
    check(same(run([(100, 40, 100, 30)]), [(128, 50, 64, 1)]))
    // Two rows apart: two rectangles; adjacent rows merge with the union of their tiles.
    px[42 * pitch / 4 + 70] = 1; px[60 * pitch / 4 + 200] = 2; px[61 * pitch / 4 + 130] = 3
    let two = run([(64, 40, 192, 30)])
    check(same(two, [(64, 42, 64, 1), (128, 60, 128, 2)]), "\(two)")
    // The partial tile at the right edge.
    px[10 * pitch / 4 + 299] = 4
    check(same(run([(290, 0, 10, 20)]), [(256, 10, 44, 1)]))
    // Overlapping rectangles: a tile is reported once.
    px[100 * pitch / 4 + 10] = 5
    check(same(run([(0, 90, 50, 20), (0, 95, 60, 10)]), [(0, 100, 64, 1)]))
    // Rectangles outside the frame are clipped; empty ones dropped.
    check(run([(400, 0, 10, 10), (0, 250, 10, 10), (0, 0, 0, 5)]).isEmpty)
    // After a reset everything is new again.
    f.reset()
    check(same(run([(0, 0, 300, 200)]), [(0, 0, 300, 200)]))
    // Every other row changed: a rectangle each. Then more than 255: the frame.
    for y in stride(from: 0, to: 200, by: 2) { px[y * pitch / 4] &+= 1; px[y * pitch / 4 + 299] &+= 1 }
    let many = (0..<200).map { (UInt32(0), UInt32($0), UInt32(300), UInt32(1)) }
    let r = run(many)
    check(r.count == 100 && r.enumerated().allSatisfy { $0.element == (0, UInt32($0.offset * 2), 300, 1) }, "\(r.count)")
    for y in 0..<200 { px[y * pitch / 4 + 70] &+= 1; px[y * pitch / 4 + 200] &+= 1 }
    let split = run((0..<200).flatMap { [(UInt32(64), UInt32($0), UInt32(64), UInt32(1)), (UInt32(192), UInt32($0), UInt32(64), UInt32(1))] })
    check(split.count == 1 && split[0] == (0, 0, 300, 200), "\(split.count)")
    // Every single-bit change in a tile changes its hash.
    var seg = [UInt8](repeating: 0x5a, count: 256)
    let h0 = seg.withUnsafeBytes { DamageFilter.hash($0.baseAddress!, bytes: 256) }
    check(h0 & 1 == 1)
    for byte in 0..<256 {
        for bit in 0..<8 {
            seg[byte] ^= UInt8(1 << bit)
            check(seg.withUnsafeBytes { DamageFilter.hash($0.baseAddress!, bytes: 256) } != h0, "byte \(byte) bit \(bit)")
            seg[byte] ^= UInt8(1 << bit)
        }
    }
    check(seg.withUnsafeBytes { DamageFilter.hash($0.baseAddress!, bytes: 252) } != h0, "length counts")
    // What hashing a whole 2560x1440 frame costs (reported, not checked).
    let bw = 2560, bh = 1440
    var big = [UInt32](repeating: 0, count: bw * bh)
    for i in big.indices { big[i] = UInt32(truncatingIfNeeded: i &* 2654435761) }
    var bf = DamageFilter(width: bw, height: bh)
    let start = DispatchTime.now().uptimeNanoseconds
    var kept = 0
    for k in 0..<10 {
        big[k] &+= 1
        kept += big.withUnsafeBytes { bf.filter([(0, 0, UInt32(bw), UInt32(bh))], base: $0.baseAddress!, pitch: bw * 4) }.rects.count
    }
    let ns = Double(DispatchTime.now().uptimeNanoseconds - start) / 10
    print(String(format: "damage filter: a whole 2560x1440 frame in %.2f ms (%.1f GB/s)", ns / 1e6, Double(bw * bh * 4) / ns))
    check(kept == 10)
}
print("PASS damage filter: unchanged tiles dropped, changed ones kept whole, edges and overlaps")

// Scrolling: rows found shifted become moves; only the exposed rows and
// rows that do not match are damage.
do {
    let w = 640, h = 480, pitch = w * 4
    var px = [UInt32](repeating: 0, count: w * h)
    /// Document line @line across the row, with a header of @header rows above @top.
    func draw(offset: Int, top: Int = 0, viewX: Int = 0, viewW: Int = 640, header: UInt32 = 7) {
        for y in 0..<h {
            for x in 0..<w {
                if y < top { px[y * w + x] = header &* 31 &+ UInt32(x) }
                else if x >= viewX && x < viewX + viewW {
                    px[y * w + x] = UInt32(truncatingIfNeeded: (offset + y) &* 2654435761) ^ UInt32(x << 12)
                } else { px[y * w + x] = 0x00445566 }
            }
        }
    }
    var f = DamageFilter(width: w, height: h)
    func run(_ rects: [DamageFilter.Rect]) -> DamageFilter.Damage {
        px.withUnsafeBytes { f.filter(rects, base: $0.baseAddress!, pitch: pitch) }
    }
    func sameRects(_ a: [DamageFilter.Rect], _ b: [DamageFilter.Rect]) -> Bool { a.elementsEqual(b, by: ==) }
    func sameMoves(_ a: [DamageFilter.Move], _ b: [DamageFilter.Move]) -> Bool { a.elementsEqual(b, by: ==) }
    let all: DamageFilter.Rect = (0, 0, UInt32(w), UInt32(h))

    // The whole frame scrolls down 30 rows (content moves up): one move, the exposed 30 rows.
    draw(offset: 0); _ = run([all])
    draw(offset: 30)
    var d = run([all])
    check(sameMoves(d.moves, [(0, 0, 640, 450, 30)]) && sameRects(d.rects, [(0, 450, 640, 30)]), "\(d.moves) \(d.rects)")
    // Up 12 rows: the move goes the other way; the exposed rows are at the top.
    draw(offset: 18)
    d = run([all])
    check(sameMoves(d.moves, [(0, 12, 640, 468, 0)]) && sameRects(d.rects, [(0, 0, 640, 12)]), "\(d.moves) \(d.rects)")
    // A fixed header of 40 rows: unchanged, so neither moved nor damaged.
    f.reset()
    draw(offset: 0, top: 40); _ = run([all])
    draw(offset: 25, top: 40)
    d = run([all])
    check(sameMoves(d.moves, [(0, 40, 640, 415, 65)]) && sameRects(d.rects, [(0, 455, 640, 25)]), "\(d.moves) \(d.rects)")
    // The header changes too (a clock): its rows are damage, in their tiles.
    draw(offset: 35, top: 40, header: 8)
    d = run([all])
    check(sameMoves(d.moves, [(0, 40, 640, 430, 50)]) && sameRects(d.rects, [(0, 0, 640, 40), (0, 470, 640, 10)]),
          "\(d.moves) \(d.rects)")
    // A row that does not match its shifted source (an edit while scrolling): damage, the moves around it.
    draw(offset: 45, top: 40, header: 8)
    for x in 0..<w { px[200 * w + x] ^= 0xff }
    d = run([all])
    check(sameMoves(d.moves, [(0, 40, 640, 160, 50), (0, 201, 640, 269, 211)]) &&
          sameRects(d.rects, [(0, 200, 640, 1), (0, 470, 640, 10)]), "\(d.moves) \(d.rects)")
    // A view inside a window (columns 100-499, rows 40-479), damage just the view:
    // the move covers the view's whole tiles.
    f.reset()
    draw(offset: 0, top: 40, viewX: 128, viewW: 384); _ = run([all])
    draw(offset: 8, top: 40, viewX: 128, viewW: 384)
    d = run([(128, 40, 384, 440)])
    check(sameMoves(d.moves, [(128, 40, 384, 432, 48)]) && sameRects(d.rects, [(128, 472, 384, 8)]), "\(d.moves) \(d.rects)")
    // Rows not known (never hashed) are never a move's source.
    f.reset()
    draw(offset: 0); _ = run([(0, 0, 640, 240)])
    draw(offset: 100)
    d = run([all])
    check(d.moves.allSatisfy { Int($0.srcY) + Int($0.h) <= 240 }, "\(d.moves)")
    // With scroll detection off, the same scroll is damage only (no moves).
    f.reset(); f.detectScroll = false
    draw(offset: 0); _ = run([all])
    draw(offset: 30)
    d = run([all])
    check(d.moves.isEmpty && sameRects(d.rects, [all]), "\(d.moves) \(d.rects)")
    f.detectScroll = true
    // Blank rows (all alike) do not vote; a scroll of a blank page is not a move.
    f.reset()
    for i in px.indices { px[i] = 0x00ffffff }
    _ = run([all])
    d = run([all])
    check(d.isEmpty)
    // The edges: a scroll by one row, sources and destinations at the frame's first and last rows.
    f.reset()
    draw(offset: 0); _ = run([all])
    draw(offset: 1)
    d = run([all])
    check(sameMoves(d.moves, [(0, 0, 640, 479, 1)]) && sameRects(d.rects, [(0, 479, 640, 1)]), "\(d.moves) \(d.rects)")
    draw(offset: 0)
    d = run([all])
    check(sameMoves(d.moves, [(0, 1, 640, 479, 0)]) && sameRects(d.rects, [(0, 0, 640, 1)]), "\(d.moves) \(d.rects)")
    // Applying moves then rectangles to what was given reproduces the frame (a model of the driver).
    f.reset()
    var screen = [UInt32](repeating: 0, count: w * h)
    func apply(_ d: DamageFilter.Damage) {
        let before = screen
        for m in d.moves { for y in 0..<Int(m.h) { for x in Int(m.x)..<Int(m.x + m.w) {
            screen[(Int(m.y) + y) * w + x] = before[(Int(m.srcY) + y) * w + x] } } }
        for r in d.rects { for y in Int(r.y)..<Int(r.y + r.h) { for x in Int(r.x)..<Int(r.x + r.w) {
            screen[y * w + x] = px[y * w + x] } } }
    }
    draw(offset: 0, top: 40); apply(run([all]))
    var offset = 0
    for step in [3, 17, 40, 1, 120, -9, -64, 25, 7, -1, 300, -200] {
        offset += step
        draw(offset: offset, top: 40, header: UInt32(offset & 3))
        apply(run([all]))
        check(screen == px, "after a scroll of \(step)")
    }
}
print("PASS scroll detection: moves for shifted rows, header and edits as damage, edges, unknown rows")

// Latency percentiles and the type workload's marks.
do {
    var s = LatencySeries()
    check(s.percentile(0.5) == 0 && s.summary.hasSuffix("(0)"))
    for v in [5, 1, 4, 2, 3, 100, 6, 7, 8, 9] { s.add(UInt64(v) * 1_000_000) }
    check(s.percentile(0.5) == 5_000_000 && s.percentile(0.9) == 9_000_000 && s.percentile(1) == 100_000_000)
    check(s.summary == "p50 5.00 ms, p90 9.00 ms, max 100.00 ms (10)", s.summary)
    for seq in 0..<64 {
        // White and black as a colour conversion might leave them.
        let pixels = (0..<4).map { typeMarkBit(seq, $0) ? 231 : 19 }
        check(typeMarkIndex(brightness: pixels) == seq % 16, "\(seq)")
    }
}
print("PASS latency series and type marks")

// "Disconnect GPU": the choice persists (and old files read as connected);
// the readiness to unplug follows the daemon and the driver's clients.
do {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("mlg-disconnect-\(getpid())")
    try? FileManager.default.removeItem(at: dir)
    let url = dir.appendingPathComponent("displays.json")
    try! FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try! Data(#"{"off":["DEL-40DD-1"]}"#.utf8).write(to: url)
    var prefs = DisplayPrefs.load(from: url)
    check(prefs.off == ["DEL-40DD-1"] && !prefs.disconnected, "an old file reads as connected")
    prefs.disconnected = true
    try! prefs.save(to: url)
    check(DisplayPrefs.load(from: url).disconnected)
    try! Data(#"{"daemon":7,"driverAttached":true,"monitors":[]}"#.utf8).write(to: dir.appendingPathComponent("s.json"))
    check(DisplayStatus.load(from: dir.appendingPathComponent("s.json")) == DisplayStatus(daemon: 7, driverAttached: true))
    let mirroring = DisplayStatus(daemon: 7, driverAttached: true, monitors: [], disconnected: false)
    let released = DisplayStatus(daemon: 7, driverAttached: true, monitors: [], disconnected: true)
    check(DisconnectReadiness.evaluate(status: mirroring, clients: [], selfPID: 1) == .waitingForDisplays)
    check(DisconnectReadiness.evaluate(status: released, clients: ["pid 1, MacLinuxGPUHost"], selfPID: 1) == .safe)
    // The driver's session closed: safe, whatever programs keep the
    // driver open (they are told the GPU was disconnected if they use it).
    let lost = DisconnectReadiness.evaluate(status: released, clients: ["pid 1, MacLinuxGPUHost", "pid 692, amdgpu_mtopg"],
                                            selfPID: 1)
    check(lost == .safeWithClients(["amdgpu_mtopg (pid 692)"]) && lost.canUnplug)
    // The device stays up across programs: an open session is what
    // Disconnect GPU closes, with programs on it or none.
    let up = DisconnectReadiness.evaluate(status: released, clients: ["pid 1, MacLinuxGPUHost", "pid 900, lse-server"],
                                          selfPID: 1, sessionOpen: true)
    check(up == .appsConnected(["lse-server (pid 900)"]) && !up.canUnplug)
    check(DisconnectReadiness.evaluate(status: released, clients: [], selfPID: 1, sessionOpen: true) == .appsConnected([]))
    check(DisconnectReadiness.evaluate(status: DisplayStatus(), clients: [], selfPID: 1) == .safe, "no daemon running")
    check(DisconnectReadiness.evaluate(status: released, clients: [], selfPID: 1, sessionBusy: true,
                                       sessionOpen: true) == .driverClosing)
    check(!DisconnectReadiness.waitingForDisplays.canUnplug && !DisconnectReadiness.driverClosing.canUnplug)
    check(up.message == "Using the GPU: lse-server (pid 900). Disconnecting closes the GPU for it.")
    check(lost.message.hasPrefix("The GPU can be unplugged now. Still connected to the driver"))
    try? FileManager.default.removeItem(at: dir)
}
print("PASS disconnect GPU: the choice persists; readiness follows the daemon and the driver's session, naming the programs it closes for")

// GPU recovery: what the daemon does about a change of LRST.
do {
    let base = ResetState(generation: 0, flags: 0, queueResets: 0, vramLost: 0, lastResult: 0)
    var queueReset = base
    queueReset.generation = 1; queueReset.queueResets = 1
    var vramLost = base
    vramLost.generation = 1; vramLost.flags = ResetState.flagLastVRAMLost; vramLost.vramLost = 1
    var wedged = base
    wedged.generation = 1; wedged.flags = ResetState.flagWedged; wedged.lastResult = -110
    check(ResetAction.evaluate(previous: nil, current: base) == .none, "the first read only records")
    check(ResetAction.evaluate(previous: nil, current: queueReset) == .none, "a new instance's history is not ours")
    check(ResetAction.evaluate(previous: base, current: base) == .none)
    check(ResetAction.evaluate(previous: base, current: queueReset) == .restartEnded)
    check(ResetAction.evaluate(previous: base, current: vramLost) == .remirror)
    check(ResetAction.evaluate(previous: nil, current: wedged) == .wedged, "wedged on the first read too")
    check(ResetAction.evaluate(previous: wedged, current: wedged) == .wedged, "wedged until power-cycled")
    check(wedged.wedged && !wedged.lastVRAMLost && vramLost.lastVRAMLost)
    // The status carries it to the menu bar; files from before read as not wedged.
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("mlg-reset-\(getpid())")
    let url = dir.appendingPathComponent("s.json")
    let status = DisplayStatus(daemon: 7, driverAttached: true, monitors: [], disconnected: true, gpuWedged: true)
    try? status.save(to: url)
    check(DisplayStatus.load(from: url) == status && status.summary == .error)
    try? Data(#"{"daemon": 7, "driverAttached": true, "monitors": []}"#.utf8).write(to: url)
    check(DisplayStatus.load(from: url)?.gpuWedged == false)
    try? FileManager.default.removeItem(at: dir)
}
print("PASS GPU recovery: first read records, a queue reset restarts ended mirrors, lost VRAM re-mirrors, wedged lets go; status round trip")
