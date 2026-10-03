//
//  DisplayAgentRuntime.swift — the display agent on the Mac: the IOSurface
//  pinning test (display-pin-test) and the virtual display that mirrors a
//  monitor on the AMD GPU (display-agent --create). It drives selector 84's
//  frame ops (IMPORT, VERIFY, RELEASE, OUTPUT, PRESENT; session_state.h)
//  over an observer client. No step falls back to another mechanism: a
//  failure is reported with its errno and the run ends with a teardown.
//

import Foundation
import IOKit
import IOSurface
import CoreGraphics
import CoreMedia
import CoreVideo
import ScreenCaptureKit

private let kIOReturnNotReadyValue = kern_return_t(bitPattern: 0xe00002d8)
private let kIOReturnNotPermittedValue = kern_return_t(bitPattern: 0xe00002e2)

private func callFailure(_ kr: kern_return_t, _ what: String) -> String {
    kr == kIOReturnNotReadyValue ? "\(what): the GPU is not running in an open session (use --init)" :
    kr == kIOReturnNotPermittedValue ? "\(what): not in this driver build (frame ops arrived in build 232)" :
    String(format: "%@: call failed (kr=%#x)", what, kr)
}

extension MacLinuxGPUHost {
    /// IMPORT: @length bytes at @base (the surface's memory, whole pages)
    /// as the structure input, which IOKit hands the driver as a memory
    /// descriptor over this process's pages.
    func displayImport(base: UnsafeRawPointer, length: Int, width: Int, height: Int, pitch: Int)
        -> (kern_return_t, Int64, UInt32) {
        guard isOpen else { return (kIOReturnError, 0, 0) }
        let geometry = UInt64(width) << 48 | UInt64(height) << 32 | UInt64(UInt32(pitch))
        let scalars: [UInt64] = [DisplayOp.importSurface.rawValue, geometry, kDisplayConfirm]
        var out = [UInt64](repeating: 0, count: 2)
        var outCount: UInt32 = 2
        var outSize = 0
        let kr = scalars.withUnsafeBufferPointer { s in
            out.withUnsafeMutableBufferPointer { o in
                IOConnectCallMethod(ucConn, kSelDisplay, s.baseAddress, 3, base, length,
                                    o.baseAddress, &outCount, nil, &outSize)
            }
        }
        guard kr == kIOReturnSuccess, outCount >= 2 else { return (kr, 0, 0) }
        return (kr, Int64(bitPattern: out[0]), UInt32(truncatingIfNeeded: out[1]))
    }

    func displayVerify(handle: UInt32, seed: UInt32) -> (kern_return_t, Int64, SurfaceVerifyResult?) {
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [DisplayOp.verify.rawValue,
                                            UInt64(handle) << 32 | UInt64(seed), kDisplayConfirm],
                                            inData: Data(), outScalars: 2, outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), SurfaceVerifyResult(data))
    }

    func displayRelease(handle: UInt32) -> (kern_return_t, Int64) {
        let (kr, values, _) = callMethod(kSelDisplay, inScalars: [DisplayOp.release.rawValue, UInt64(handle),
                                         kDisplayConfirm], inData: Data(), outScalars: 2, outSize: 0)
        return (kr, values.first.map { Int64(bitPattern: $0) } ?? 0)
    }

    func displayOutput(connector: String, width: Int, height: Int, refreshMilliHz: Int)
        -> (kern_return_t, Int64, DisplayReport?) {
        var request = Data(count: 40)
        request.withUnsafeMutableBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            for (i, c) in connector.utf8.prefix(31).enumerated() { bytes[i] = c }
            raw.storeBytes(of: UInt32(width).littleEndian, toByteOffset: 32, as: UInt32.self)
            raw.storeBytes(of: UInt32(height).littleEndian, toByteOffset: 36, as: UInt32.self)
        }
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [DisplayOp.output.rawValue,
                                            UInt64(refreshMilliHz), kDisplayConfirm],
                                            inData: request, outScalars: 2, outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), DisplayReport(data))
    }

    func displayPresent(handle: UInt32, rects: [(x: UInt32, y: UInt32, w: UInt32, h: UInt32)])
        -> (kern_return_t, Int64, PresentStats?) {
        var request = Data(count: 8 + rects.count * 16)
        request.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: UInt32(rects.count).littleEndian, toByteOffset: 0, as: UInt32.self)
            for (i, r) in rects.enumerated() {
                for (j, v) in [r.x, r.y, r.w, r.h].enumerated() {
                    raw.storeBytes(of: v.littleEndian, toByteOffset: 8 + i * 16 + j * 4, as: UInt32.self)
                }
            }
        }
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [DisplayOp.present.rawValue, UInt64(handle),
                                            kDisplayConfirm], inData: request, outScalars: 2,
                                            outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), PresentStats(data))
    }
}

private func option(_ options: [String], _ flag: String) -> String? {
    guard let at = options.firstIndex(of: flag), at + 1 < options.count else { return nil }
    return options[at + 1]
}

/// The session client (--init) and the observer the agent uses.
private func openClients(_ options: [String]) -> (session: MacLinuxGPUHost?, observer: MacLinuxGPUHost)? {
    var session: MacLinuxGPUHost?
    if options.contains("--init") {
        let host = MacLinuxGPUHost()
        guard host.openUserClient(), host.initDevice() else {
            print("ERROR: GPU initialization failed (scripts/read-driver-log.py shows why)")
            return nil
        }
        session = host
    }
    let observer = MacLinuxGPUHost()
    guard observer.openUserClient(observer: true) else {
        print("ERROR: failed to open an observer client")
        session?.closeUserClient()
        return nil
    }
    return (session, observer)
}

private func makeSurface(width: Int, height: Int) -> IOSurfaceRef? {
    let properties: [String: Any] = [
        kIOSurfaceWidth as String: width, kIOSurfaceHeight as String: height,
        kIOSurfaceBytesPerElement as String: 4,
        kIOSurfacePixelFormat as String: UInt32(kCVPixelFormatType_32BGRA),
    ]
    return IOSurfaceCreate(properties as CFDictionary)
}

private func fill(_ surface: IOSurfaceRef, seed: UInt32) {
    IOSurfaceLock(surface, [], nil)
    let words = IOSurfaceGetAllocSize(surface) / 4
    let base = IOSurfaceGetBaseAddress(surface).bindMemory(to: UInt32.self, capacity: words)
    for d in 0..<words { base[d] = surfacePattern(seed: seed, dword: UInt64(d)) }
    IOSurfaceUnlock(surface, [], nil)
}

// ----------------------------------------------------------------
// MARK: - display-pin-test
// ----------------------------------------------------------------

/// Does the GPU keep seeing what this process writes into an IOSurface
/// after the call that imported it returned? The surface is imported once
/// (the driver maps its pages for the GPU), then rewritten from the CPU
/// each second; each time the driver reads it with SDMA (and through its
/// own CPU view of the same pages) and compares with the new pattern, and
/// with the previous one, which must no longer match.
func runDisplayPinTest(_ options: [String]) -> Int32 {
    let seconds = max(1, Int(option(options, "--seconds") ?? "") ?? 10)
    let width = Int(option(options, "--width") ?? "") ?? 2560
    let height = Int(option(options, "--height") ?? "") ?? 1440
    guard let (session, observer) = openClients(options) else { return 1 }
    defer { observer.closeUserClient(); session?.closeUserClient() }
    guard let surface = makeSurface(width: width, height: height) else {
        print("pin-test: IOSurfaceCreate(\(width)x\(height) BGRA) failed")
        return 1
    }
    let length = IOSurfaceGetAllocSize(surface), pitch = IOSurfaceGetBytesPerRow(surface)
    guard length % 16384 == 0 else {
        print("pin-test: the surface's allocation (\(length) bytes) is not whole 16 KiB pages")
        return 1
    }
    print("pin-test: IOSurface \(IOSurfaceGetID(surface)) \(width)x\(height), pitch \(pitch), \(length) bytes")
    fill(surface, seed: 1)
    IOSurfaceLock(surface, .readOnly, nil)
    let (kr, status, handle) = observer.displayImport(base: UnsafeRawPointer(IOSurfaceGetBaseAddress(surface)),
                                                      length: length, width: width, height: height,
                                                      pitch: pitch)
    IOSurfaceUnlock(surface, .readOnly, nil)
    guard kr == kIOReturnSuccess else { print("pin-test: " + callFailure(kr, "IMPORT")); return 1 }
    guard status == 0, handle != 0 else { print("pin-test: IMPORT: \(displayErrno(status))"); return 1 }
    print("pin-test: imported as handle \(handle); the call has returned")

    var failures = 0
    func check(_ seed: UInt32, expectMatch: Bool, label: String) {
        let (vkr, vstatus, result) = observer.displayVerify(handle: handle, seed: seed)
        guard vkr == kIOReturnSuccess, vstatus == 0, let result else {
            print("pin-test: \(label): " + (vkr == kIOReturnSuccess ? "VERIFY: \(displayErrno(vstatus))" :
                                             callFailure(vkr, "VERIFY")))
            failures += 1
            return
        }
        let ok = expectMatch ? result.gpuMismatches == 0 && result.cpuMismatches == 0 && result.cpuChecked :
                               result.gpuMismatches == result.dwords
        print(String(format: "pin-test: %@ seed %u: GPU %u, CPU %u of %u dwords differ (%.0f us, GART 0x%llx)%@",
                     label, seed, result.gpuMismatches, result.cpuMismatches, result.dwords,
                     Double(result.gpuNs) / 1000, result.gpuAddress,
                     ok ? "" : String(format: " — FAIL (first GPU mismatch at %llu: 0x%08x, want 0x%08x)",
                                      result.firstGPUMismatch, result.gpuValue, result.expectedValue)))
        if !ok { failures += 1 }
    }
    check(1, expectMatch: true, label: "after import")
    for second in 1...seconds {
        Thread.sleep(forTimeInterval: 1)
        let seed = UInt32(second + 1)
        fill(surface, seed: seed)
        check(seed, expectMatch: true, label: "t+\(second)s new")
        check(seed - 1, expectMatch: false, label: "t+\(second)s old")
    }
    let (rkr, rstatus) = observer.displayRelease(handle: handle)
    print("pin-test: RELEASE -> " + (rkr == kIOReturnSuccess ? displayErrno(rstatus) : callFailure(rkr, "RELEASE")))
    if rkr != kIOReturnSuccess || rstatus != 0 { failures += 1 }
    let (_, gone, _) = observer.displayVerify(handle: handle, seed: 1)
    if gone != -Int64(ENOENT) { print("pin-test: the handle still answers after RELEASE"); failures += 1 }
    print(failures == 0 ? "pin-test: PASS — the GPU sees every write after the import call returned" :
                          "pin-test: FAIL (\(failures) check(s))")
    return failures == 0 ? 0 : 1
}

// ----------------------------------------------------------------
// MARK: - display-agent --create
// ----------------------------------------------------------------

/// One monitor mirrored as a macOS display: a CGVirtualDisplay described
/// from the monitor, captured with ScreenCaptureKit, each frame's damage
/// copied onto the monitor by the driver (IMPORT once per surface of the
/// capture pool, PRESENT per frame).
private final class MirroredDisplay: NSObject, SCStreamOutput, SCStreamDelegate {
    let observer: MacLinuxGPUHost
    let plan: VirtualDisplayPlan
    let queue = DispatchQueue(label: "MacLinuxGPU.display-agent.frames")
    var display: CGVirtualDisplay?
    var stream: SCStream?
    var mode: VirtualDisplayPlan.Mode?
    var handles: [IOSurfaceID: UInt32] = [:]
    var frames = 0, presented = 0, copyNs: UInt64 = 0, flipNs: UInt64 = 0, bytes: UInt64 = 0
    var failure: String?
    var stopped = false

    init(observer: MacLinuxGPUHost, plan: VirtualDisplayPlan) {
        self.observer = observer
        self.plan = plan
    }

    func createDisplay() -> Bool {
        let descriptor = CGVirtualDisplayDescriptor()
        descriptor.name = plan.name
        descriptor.vendorID = plan.vendorID
        descriptor.productID = plan.productID
        descriptor.serialNum = plan.serialNum
        descriptor.sizeInMillimeters = CGSize(width: plan.sizeInMillimeters.width,
                                              height: plan.sizeInMillimeters.height)
        descriptor.maxPixelsWide = UInt32(plan.maxPixelsWide)
        descriptor.maxPixelsHigh = UInt32(plan.maxPixelsHigh)
        descriptor.redPrimary = CGPoint(x: plan.redPrimary.x, y: plan.redPrimary.y)
        descriptor.greenPrimary = CGPoint(x: plan.greenPrimary.x, y: plan.greenPrimary.y)
        descriptor.bluePrimary = CGPoint(x: plan.bluePrimary.x, y: plan.bluePrimary.y)
        descriptor.whitePoint = CGPoint(x: plan.whitePoint.x, y: plan.whitePoint.y)
        descriptor.queue = DispatchQueue(label: "MacLinuxGPU.display-agent.virtual-display")
        descriptor.terminationHandler = { _, _ in print("display-agent: macOS ended the virtual display") }
        guard let display = CGVirtualDisplay(descriptor: descriptor) else {
            failure = "CGVirtualDisplay could not be created"
            return false
        }
        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = UInt32(plan.hiDPI)
        settings.modes = plan.modes.map {
            CGVirtualDisplayMode(width: UInt32($0.width), height: UInt32($0.height), refreshRate: $0.refreshRate)
        }
        guard display.apply(settings) else {
            failure = "CGVirtualDisplay rejected the mode list"
            return false
        }
        self.display = display
        // macOS brings the display online asynchronously.
        for _ in 0..<50 {
            var count: UInt32 = 0
            CGGetOnlineDisplayList(0, nil, &count)
            var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
            CGGetOnlineDisplayList(count, &ids, &count)
            if ids.contains(display.displayID) { return true }
            Thread.sleep(forTimeInterval: 0.1)
        }
        failure = "the virtual display \(display.displayID) did not come online"
        return false
    }

    /// The plan's mode macOS currently uses for the virtual display.
    func currentMode() -> VirtualDisplayPlan.Mode? {
        guard let display, let cg = CGDisplayCopyDisplayMode(display.displayID) else { return nil }
        return plan.modes.filter { $0.width == cg.pixelWidth && $0.height == cg.pixelHeight }
            .min { abs($0.refreshRate - cg.refreshRate) < abs($1.refreshRate - cg.refreshRate) }
    }

    func startOutput(_ mode: VirtualDisplayPlan.Mode) -> Bool {
        let mhz = Int((mode.refreshRate * 1000).rounded())
        let (kr, status, _) = observer.displayOutput(connector: plan.connector, width: mode.width,
                                                     height: mode.height, refreshMilliHz: mhz)
        guard kr == kIOReturnSuccess, status == 0 else {
            failure = kr == kIOReturnSuccess ? "OUTPUT \(mode.width)x\(mode.height)@\(mhz) mHz: \(displayErrno(status))" :
                                               callFailure(kr, "OUTPUT")
            return false
        }
        self.mode = mode
        print(String(format: "display-agent: %@ lit at %dx%d @ %.3f Hz", plan.connector, mode.width,
                     mode.height, mode.refreshRate))
        return true
    }

    func startCapture(_ mode: VirtualDisplayPlan.Mode) -> Bool {
        guard let display else { return false }
        let found = DispatchSemaphore(value: 0)
        var scDisplay: SCDisplay?, error: Error?
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: false) { content, e in
            scDisplay = content?.displays.first { $0.displayID == display.displayID }
            error = e
            found.signal()
        }
        found.wait()
        guard let scDisplay else {
            failure = "ScreenCaptureKit does not list the virtual display" + (error.map { " (\($0))" } ?? "")
            return false
        }
        let configuration = SCStreamConfiguration()
        configuration.width = mode.width
        configuration.height = mode.height
        configuration.pixelFormat = kCVPixelFormatType_32BGRA
        configuration.minimumFrameInterval = CMTime(value: 1000, timescale: CMTimeScale((mode.refreshRate * 1000).rounded()))
        configuration.queueDepth = 4
        configuration.showsCursor = true
        let stream = SCStream(filter: SCContentFilter(display: scDisplay, excludingWindows: []),
                              configuration: configuration, delegate: self)
        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        } catch {
            failure = "ScreenCaptureKit output: \(error)"
            return false
        }
        let started = DispatchSemaphore(value: 0)
        var startError: Error?
        stream.startCapture { startError = $0; started.signal() }
        started.wait()
        if let startError {
            failure = "ScreenCaptureKit start: \(startError)"
            return false
        }
        self.stream = stream
        return true
    }

    func stopCapture() {
        guard let stream else { return }
        let done = DispatchSemaphore(value: 0)
        stream.stopCapture { _ in done.signal() }
        _ = done.wait(timeout: .now() + 5)
        self.stream = nil
        queue.sync { stopped = true }
    }

    func releaseImports() {
        queue.sync {
            for handle in handles.values { _ = observer.displayRelease(handle: handle) }
            handles.removeAll()
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        queue.async { self.failure = self.failure ?? "ScreenCaptureKit stopped: \(error)" }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, !stopped, failure == nil, let mode,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false)
                as? [[SCStreamFrameInfo: Any]], let info = attachments.first,
              let raw = info[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
              let pixels = CMSampleBufferGetImageBuffer(sample),
              let surface = CVPixelBufferGetIOSurface(pixels)?.takeUnretainedValue() else { return }
        frames += 1
        let id = IOSurfaceGetID(surface)
        var handle = handles[id]
        if handle == nil {
            let width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface)
            guard width == mode.width, height == mode.height else { return } // a frame of the old mode
            IOSurfaceLock(surface, .readOnly, nil)
            let (kr, status, h) = observer.displayImport(base: UnsafeRawPointer(IOSurfaceGetBaseAddress(surface)),
                                                         length: IOSurfaceGetAllocSize(surface), width: width,
                                                         height: height, pitch: IOSurfaceGetBytesPerRow(surface))
            IOSurfaceUnlock(surface, .readOnly, nil)
            guard kr == kIOReturnSuccess, status == 0, h != 0 else {
                failure = kr == kIOReturnSuccess ? "IMPORT of capture surface \(id): \(displayErrno(status))" :
                                                   callFailure(kr, "IMPORT")
                return
            }
            handles[id] = h
            handle = h
            print("display-agent: capture surface \(id) imported as handle \(h)")
        }
        let dirty = (info[.dirtyRects] as? [NSDictionary] ?? []).compactMap { CGRect(dictionaryRepresentation: $0) }
        let rects = presentRects(dirty, width: mode.width, height: mode.height)
        if rects.isEmpty { return }
        let (kr, status, stats) = observer.displayPresent(handle: handle!, rects: rects)
        guard kr == kIOReturnSuccess, status == 0, let stats else {
            failure = kr == kIOReturnSuccess ?
                "PRESENT: \(displayErrno(status))" + (stats.map { " (copy \($0.copyStatus), flip \($0.flipStatus))" } ?? "") :
                callFailure(kr, "PRESENT")
            return
        }
        presented += 1
        copyNs += stats.copyNs
        flipNs += stats.flipNs
        bytes += stats.bytesCopied
    }
}

/// display-agent --create: one monitor on the AMD GPU becomes a macOS
/// display for --seconds (default until Ctrl-C), then everything is undone:
/// capture stopped, imports released, the monitor's previous configuration
/// restored, the virtual display removed.
func runDisplayAgentCreate(_ options: [String]) -> Int32 {
    let seconds = Double(option(options, "--seconds") ?? "") ?? 0
    guard CGPreflightScreenCaptureAccess() else {
        _ = CGRequestScreenCaptureAccess()
        print("display-agent: Screen Recording permission is needed to capture the virtual display.")
        print("  Allow it for the app running this command (System Settings › Privacy & Security ›")
        print("  Screen & System Audio Recording), then run the command again.")
        return 1
    }
    guard let (session, observer) = openClients(options) else { return 1 }
    defer { observer.closeUserClient(); session?.closeUserClient() }

    let (pkr, pstatus, probed) = observer.display(.probe)
    guard pkr == kIOReturnSuccess, let probed else { print("display-agent: " + callFailure(pkr, "PROBE")); return 1 }
    if pstatus != 0 { print("display-agent: probe: \(displayErrno(pstatus))") }
    let wanted = option(options, "--connector")
    guard let connector = probed.connectors.first(where: { $0.connected && (wanted == nil || $0.name == wanted) }) else {
        print("display-agent: no connected monitor" + (wanted.map { " named \($0)" } ?? ""))
        return 1
    }
    let (mkr, mstatus, modes) = observer.displayModes(connector.name)
    guard mkr == kIOReturnSuccess, mstatus == 0, let modes else {
        print("display-agent: \(connector.name): " + (mkr == kIOReturnSuccess ? displayErrno(mstatus) : callFailure(mkr, "MODES")))
        return 1
    }
    let plan: VirtualDisplayPlan
    switch planVirtualDisplay(connector: connector.name, modes: modes, edid: observer.connectorEDID(connector.name)) {
    case .success(let p): plan = p
    case .failure(let error): print("display-agent: \(connector.name): \(error)"); return 1
    }
    plan.lines.forEach { print($0) }

    let mirror = MirroredDisplay(observer: observer, plan: plan)
    var outputOn = false
    func teardown() -> Int32 {
        mirror.stopCapture()
        mirror.releaseImports()
        if outputOn {
            let (kr, status, _) = observer.display(.off)
            print("display-agent: monitor restored: " + (kr == kIOReturnSuccess ? displayErrno(status) : callFailure(kr, "OFF")))
        }
        mirror.display = nil
        print("display-agent: virtual display removed")
        var usage = rusage()
        getrusage(RUSAGE_SELF, &usage)
        let cpu = Double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                  Double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6
        let n = max(mirror.presented, 1)
        print(String(format: "display-agent: %d frame(s) captured, %d presented; copy %.2f ms, flip %.2f ms on average; %.1f MB copied; agent CPU %.2f s",
                     mirror.frames, mirror.presented, Double(mirror.copyNs) / Double(n) / 1e6,
                     Double(mirror.flipNs) / Double(n) / 1e6, Double(mirror.bytes) / 1e6, cpu))
        if let failure = mirror.failure {
            print("display-agent: FAILED: \(failure)")
            return 1
        }
        return 0
    }

    guard mirror.createDisplay() else { return teardown() }
    print("display-agent: virtual display \(mirror.display!.displayID) \"\(plan.name)\" is online")
    guard let mode = mirror.currentMode() else {
        mirror.failure = "macOS uses a mode for the virtual display that the monitor did not list"
        return teardown()
    }
    guard mirror.startOutput(mode) else { return teardown() }
    outputOn = true
    guard mirror.startCapture(mode) else { return teardown() }
    print("display-agent: mirroring; " + (seconds > 0 ? "for \(Int(seconds)) s" : "Ctrl-C to stop"))

    var interrupted = false
    signal(SIGINT, SIG_IGN)
    signal(SIGTERM, SIG_IGN)
    let sources = [SIGINT, SIGTERM].map { sig -> DispatchSourceSignal in
        let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
        source.setEventHandler { interrupted = true }
        source.resume()
        return source
    }
    defer { sources.forEach { $0.cancel() } }
    let deadline = seconds > 0 ? Date().addingTimeInterval(seconds) : Date.distantFuture
    var lastReport = Date()
    while !interrupted && Date() < deadline {
        RunLoop.main.run(until: Date().addingTimeInterval(0.25))
        if mirror.queue.sync(execute: { mirror.failure }) != nil { break }
        // A mode chosen in System Settings › Displays: relight at it.
        if let now = mirror.currentMode(), now != mirror.mode {
            print(String(format: "display-agent: macOS switched to %dx%d @ %.3f Hz", now.width, now.height, now.refreshRate))
            mirror.stopCapture()
            mirror.releaseImports()
            mirror.queue.sync { mirror.stopped = false }
            guard mirror.startOutput(now), mirror.startCapture(now) else { break }
        }
        if Date().timeIntervalSince(lastReport) >= 5 {
            lastReport = Date()
            let (f, p) = mirror.queue.sync { (mirror.frames, mirror.presented) }
            print("display-agent: \(f) frame(s) captured, \(p) presented")
        }
    }
    return teardown()
}
