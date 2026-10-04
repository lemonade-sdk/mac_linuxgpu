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
import AppKit
import QuartzCore

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

    /// PRESENT: queue a frame (its damage and capture time, mach ns) for the
    /// driver's output worker; returns once queued. No rectangle and handle
    /// 0 reads the worker's statistics only.
    func displayPresent(handle: UInt32, rects: [(x: UInt32, y: UInt32, w: UInt32, h: UInt32)],
                        captureNs: UInt64) -> (kern_return_t, Int64, PresentStats?) {
        var request = Data(count: 16 + rects.count * 16)
        request.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: UInt32(rects.count).littleEndian, toByteOffset: 0, as: UInt32.self)
            raw.storeBytes(of: captureNs.littleEndian, toByteOffset: 8, as: UInt64.self)
            for (i, r) in rects.enumerated() {
                for (j, v) in [r.x, r.y, r.w, r.h].enumerated() {
                    raw.storeBytes(of: v.littleEndian, toByteOffset: 16 + i * 16 + j * 4, as: UInt32.self)
                }
            }
        }
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [DisplayOp.present.rawValue, UInt64(handle),
                                            kDisplayConfirm], inData: request, outScalars: 2,
                                            outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), PresentStats(data))
    }

    /// The worker's statistics: PRESENT with no rectangle. Drivers through
    /// build 235 admit PRESENT only with an imported surface's handle, so
    /// one is passed (the driver does not use it for a read).
    func displayStats(handle: UInt32) -> PresentStats? {
        guard handle != 0 else { return nil }
        let (kr, status, stats) = displayPresent(handle: handle, rects: [], captureNs: 0)
        return kr == kIOReturnSuccess && status == 0 ? stats : nil
    }
}

// ----------------------------------------------------------------
// MARK: - measurement helpers
// ----------------------------------------------------------------

private let machTimebase: mach_timebase_info_data_t = {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    return info
}()

/// mach_absolute_time units to ns: the clock the driver's ktime_get_ns()
/// reads (CLOCK_UPTIME_RAW), so capture-to-flip latency needs no conversion.
private func machToNs(_ t: UInt64) -> UInt64 {
    t / UInt64(machTimebase.denom) * UInt64(machTimebase.numer) +
        t % UInt64(machTimebase.denom) * UInt64(machTimebase.numer) / UInt64(machTimebase.denom)
}

private func threadCPUNs() -> UInt64 { clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) }
private func uptimeNs() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }

/// A process's CPU time and wakeups: this process from proc_pid_rusage
/// (interrupt and package-idle wakeups); another user's (the driver's)
/// through top(1), which may read them (idle wakeups only).
struct ProcessUsage {
    let cpuNs: UInt64
    let wakeups: UInt64
    let at: UInt64

    static func current() -> ProcessUsage? {
        var info = rusage_info_v4()
        let r = withUnsafeMutablePointer(to: &info) { p in
            p.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(getpid(), RUSAGE_INFO_V4, $0) }
        }
        var usage = rusage()
        guard r == 0, getrusage(RUSAGE_SELF, &usage) == 0 else { return nil }
        let cpu = UInt64(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1_000_000_000 +
                  UInt64(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000
        return ProcessUsage(cpuNs: cpu, wakeups: info.ri_interrupt_wkups + info.ri_pkg_idle_wkups, at: uptimeNs())
    }

    static func sampled(pid: pid_t) -> ProcessUsage? {
        let top = Process()
        top.executableURL = URL(fileURLWithPath: "/usr/bin/top")
        top.arguments = ["-l", "1", "-pid", String(pid), "-stats", "pid,idlew,time"]
        let pipe = Pipe()
        top.standardOutput = pipe
        top.standardError = FileHandle.nullDevice
        do { try top.run() } catch { return nil }
        let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        top.waitUntilExit()
        guard let line = output.split(separator: "\n").last(where: { $0.hasPrefix(String(pid) + " ") }) else { return nil }
        let fields = line.split(separator: " ")
        guard fields.count >= 3, let wakeups = UInt64(fields[1].filter { $0 != "+" }) else { return nil }
        // TIME: [hh:]mm:ss.cc
        let parts = fields[2].filter { $0 != "+" }.split(separator: ":").map { Double($0) ?? 0 }
        let seconds = parts.reduce(0) { $0 * 60 + $1 }
        return ProcessUsage(cpuNs: UInt64(seconds * 1e9), wakeups: wakeups, at: uptimeNs())
    }

    /// CPU per second and wakeups per second from @start to self.
    func rates(since start: ProcessUsage) -> (cpuPercent: Double, wakeupsPerSecond: Double) {
        let seconds = Double(at - start.at) / 1e9
        guard seconds > 0 else { return (0, 0) }
        return (Double(cpuNs - start.cpuNs) / 1e9 / seconds * 100, Double(wakeups - start.wakeups) / seconds)
    }
}

/// The driver's process: the one running this project's driver executable
/// (its name, as the kernel keeps it, truncated to 16 bytes) as _driverkit.
func driverProcessID(_ options: [String]) -> pid_t? {
    if let given = option(options, "--dext-pid") { return pid_t(given) }
    guard let driverkit = getpwnam("_driverkit")?.pointee.pw_uid else { return nil }
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
    var size = 0
    guard sysctl(&mib, 3, nil, &size, nil, 0) == 0 else { return nil }
    var procs = [kinfo_proc](repeating: kinfo_proc(), count: size / MemoryLayout<kinfo_proc>.stride + 8)
    size = procs.count * MemoryLayout<kinfo_proc>.stride
    guard sysctl(&mib, 3, &procs, &size, nil, 0) == 0 else { return nil }
    let want = String("com.geramyloveless.MacAMDGPUHost.MacAMDGPU".prefix(16))
    let found = procs.prefix(size / MemoryLayout<kinfo_proc>.stride).filter { p in
        var comm = p.kp_proc.p_comm
        let name = withUnsafeBytes(of: &comm) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        return p.kp_eproc.e_ucred.cr_uid == driverkit && name == want
    }
    return found.count == 1 ? found[0].kp_proc.p_pid : nil
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
        // The GPU's reads decide. The driver's CPU view of the descriptor is
        // reported only: on macOS 26 it is a snapshot taken at the import
        // call, not the client's live pages (hardware run, build 233).
        let ok = expectMatch ? result.gpuMismatches == 0 : result.gpuMismatches == result.dwords
        print(String(format: "pin-test: %@ seed %u: GPU %u (CPU view %u) of %u dwords differ (%.0f us, GART 0x%llx)%@",
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
    /// Presented frames whose buffers ScreenCaptureKit must not reuse yet,
    /// with the driver's received count once each was queued: the driver
    /// copies after PRESENT returns, so a frame's buffer is held until the
    /// driver has flipped it (its copy is done; the flip waited for it) or
    /// replaced it with a newer frame (never copied).
    var held: [(sample: CMSampleBuffer, received: UInt64)] = []
    var measurement = PresentMeasurement()
    var last: PresentStats?
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
        // macOS brings the display online, then gives it a mode,
        // asynchronously. The process sees it through CoreGraphics'
        // reconfiguration notifications (agentWindowServerSetup), which
        // arrive while the run loop turns.
        for _ in 0..<50 {
            var count: UInt32 = 0
            CGGetOnlineDisplayList(0, nil, &count)
            var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
            CGGetOnlineDisplayList(count, &ids, &count)
            if ids.contains(display.displayID) && currentMode() != nil { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }
        failure = "the virtual display \(display.displayID) did not come online in a mode the monitor listed" +
            (CGDisplayCopyDisplayMode(display.displayID).map { " (macOS chose \($0.pixelWidth)x\($0.pixelHeight) @ \($0.refreshRate) Hz)" } ?? "")
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
        configuration.queueDepth = 6 // frames held until flipped (see held) and one being captured
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
            held.removeAll()
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
        let handlerStart = threadCPUNs()
        measurement.frames += 1
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
        if rects.isEmpty { measurement.idleFrames += 1; return }
        // The frame's composition time on the virtual display.
        let captureNs = (info[.displayTime] as? UInt64).map(machToNs) ?? 0
        let callCPU = threadCPUNs(), callWall = uptimeNs()
        let (kr, status, stats) = observer.displayPresent(handle: handle!, rects: rects, captureNs: captureNs)
        let callWallNs = uptimeNs() - callWall, callCPUNs = threadCPUNs() - callCPU
        guard kr == kIOReturnSuccess, status == 0, let stats else {
            failure = kr == kIOReturnSuccess ?
                "PRESENT: \(displayErrno(status))" + (stats.map { " (worker error \($0.error))" } ?? "") :
                callFailure(kr, "PRESENT")
            return
        }
        last = stats
        measurement.observe(stats)
        held.removeAll { stats.flipped + stats.replaced >= $0.received }
        held.append((sample, stats.received))
        measurement.present(callCPUNs: callCPUNs, callWallNs: callWallNs, handlerCPUNs: threadCPUNs() - handlerStart)
    }
}

/// --workload: what the virtual display shows while it is measured.
///   still  nothing moves (the desktop as it is)
///   move   a 480x320 window crosses the display, moved every refresh
///   full   a window covering the display changes colour every refresh
///          (the damage of full-screen video)
private final class Workload {
    let kind: String
    var window: NSWindow?
    var timer: Timer?
    var tick = 0
    var position = CGPoint(x: 0, y: 0), velocity = CGPoint(x: 9, y: 6)

    init(kind: String) { self.kind = kind }

    /// nil when started; otherwise why not.
    func start(displayID: CGDirectDisplayID, refreshHz: Double) -> String? {
        if kind == "still" { return nil }
        guard kind == "move" || kind == "full" else { return "unknown workload \(kind) (still, move or full)" }
        // The app is already a background agent (runDisplayAgent).
        // CoreGraphics' global space has its origin at the top left of the
        // main display, AppKit's at the bottom left.
        let bounds = CGDisplayBounds(displayID), main = CGDisplayBounds(CGMainDisplayID())
        guard bounds.width > 0 else { return "the virtual display has no bounds" }
        let screen = NSRect(x: bounds.minX, y: main.height - bounds.maxY, width: bounds.width, height: bounds.height)
        let frame = kind == "full" ? screen : NSRect(x: screen.minX, y: screen.minY, width: 480, height: 320)
        let window = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.level = .floating
        window.ignoresMouseEvents = true
        let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.systemOrange.cgColor
        window.contentView = view
        window.setFrame(frame, display: true)
        window.orderFrontRegardless()
        self.window = window
        let interval = 1 / max(refreshHz, 1)
        let timer = Timer(timeInterval: interval, repeats: true) { [weak self] _ in self?.step(screen: screen) }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
        return nil
    }

    private func step(screen: NSRect) {
        guard let window else { return }
        tick += 1
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        if kind == "full" {
            window.contentView?.layer?.backgroundColor =
                NSColor(hue: CGFloat(tick % 240) / 240, saturation: 0.8, brightness: 0.9, alpha: 1).cgColor
        } else {
            var p = position
            p.x += velocity.x; p.y += velocity.y
            if p.x < 0 || p.x + 480 > screen.width { velocity.x = -velocity.x; p.x = min(max(p.x, 0), screen.width - 480) }
            if p.y < 0 || p.y + 320 > screen.height { velocity.y = -velocity.y; p.y = min(max(p.y, 0), screen.height - 320) }
            position = p
            window.setFrameOrigin(NSPoint(x: screen.minX + p.x, y: screen.minY + p.y))
        }
        CATransaction.commit()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        window?.orderOut(nil)
        window = nil
    }
}

/// display-agent --create: one monitor on the AMD GPU becomes a macOS
/// display for --seconds (default until Ctrl-C), then everything is undone:
/// capture stopped, imports released, the monitor's previous configuration
/// restored, the virtual display removed.
/// The agent talks to the WindowServer (CGVirtualDisplay, ScreenCaptureKit)
/// from the app's own executable: as a background agent that has finished
/// launching (never in the Dock, never bouncing), and with a display
/// reconfiguration callback registered. CoreGraphics caches the display
/// configuration a process has seen; without the callback (and the run
/// loop turning) a virtual display created after that has no modes in this
/// process (CGDisplayCopyDisplayMode is nil), which a daemon that
/// re-creates displays on hotplug would always hit.
func agentWindowServerSetup() {
    NSApplication.shared.setActivationPolicy(.accessory)
    NSApplication.shared.finishLaunching()
    CGDisplayRegisterReconfigurationCallback({ _, _, _ in }, nil)
}

/// SIGINT/SIGTERM end the agent: the mirroring stops and everything is
/// undone. Detached and launchd runs have no terminal; nothing else ends it.
private var agentInterrupted = false
private var agentSignalSources: [DispatchSourceSignal] = []
private func agentHandleSignals() {
    guard agentSignalSources.isEmpty else { return }
    signal(SIGINT, SIG_IGN)
    signal(SIGTERM, SIG_IGN)
    signal(SIGHUP, SIG_IGN)
    agentSignalSources = [SIGINT, SIGTERM].map { sig -> DispatchSourceSignal in
        let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
        source.setEventHandler { agentInterrupted = true }
        source.resume()
        return source
    }
}

private func agentLog(_ text: String) {
    let stamp = ISO8601DateFormatter().string(from: Date())
    print("\(stamp) \(text)")
    fflush(stdout)
}

func runDisplayAgentCreate(_ options: [String]) -> Int32 {
    if options.contains("--daemon") { return runDisplayAgentDaemon(options) }
    let seconds = Double(option(options, "--seconds") ?? "") ?? 0
    agentWindowServerSetup()
    agentHandleSignals()
    guard CGPreflightScreenCaptureAccess() else {
        _ = CGRequestScreenCaptureAccess()
        print("display-agent: Screen Recording permission is needed to capture the virtual display.")
        print("  Allow it for the app running this command (System Settings › Privacy & Security ›")
        print("  Screen & System Audio Recording), then run the command again.")
        return 1
    }
    guard let (session, observer) = openClients(options) else { return 1 }
    defer { observer.closeUserClient(); session?.closeUserClient() }
    let deadline = seconds > 0 ? Date().addingTimeInterval(seconds) : Date.distantFuture
    switch mirrorMonitor(observer: observer, options: options, daemon: false,
                         shouldStop: { agentInterrupted || Date() >= deadline }) {
    case .ended(let code): return code
    case .noMonitor, .monitorGone: return 1
    }
}

enum MirrorEnd { case ended(Int32), noMonitor, monitorGone }

/// One connected monitor mirrored until @shouldStop, the monitor leaving
/// (daemon: checked from the driver's cached hotplug state), or a failure.
private func mirrorMonitor(observer: MacLinuxGPUHost, options: [String], daemon: Bool,
                           shouldStop: () -> Bool) -> MirrorEnd {
    let (pkr, pstatus, probed) = observer.display(.probe)
    guard pkr == kIOReturnSuccess, let probed else { print("display-agent: " + callFailure(pkr, "PROBE")); return .ended(1) }
    if pstatus != 0 { print("display-agent: probe: \(displayErrno(pstatus))") }
    let wanted = option(options, "--connector")
    guard let connector = probed.connectors.first(where: { $0.connected && (wanted == nil || $0.name == wanted) }) else {
        print("display-agent: no connected monitor" + (wanted.map { " named \($0)" } ?? ""))
        return .noMonitor
    }
    let (mkr, mstatus, modes) = observer.displayModes(connector.name)
    guard mkr == kIOReturnSuccess, mstatus == 0, let modes else {
        print("display-agent: \(connector.name): " + (mkr == kIOReturnSuccess ? displayErrno(mstatus) : callFailure(mkr, "MODES")))
        return .ended(1)
    }
    let plan: VirtualDisplayPlan
    switch planVirtualDisplay(connector: connector.name, modes: modes, edid: observer.connectorEDID(connector.name)) {
    case .success(let p): plan = p
    case .failure(let error): print("display-agent: \(connector.name): \(error)"); return .ended(1)
    }
    if !daemon { plan.lines.forEach { print($0) } }

    let mirror = MirroredDisplay(observer: observer, plan: plan)
    let workload = Workload(kind: daemon ? "still" : option(options, "--workload") ?? "still")
    let warmup = daemon ? Double.infinity : Double(option(options, "--warmup") ?? "") ?? 2
    let dextPID = daemon ? nil : driverProcessID(options)
    var outputOn = false
    var monitorLeft = false
    // The measured window: from --warmup after mirroring starts to the end.
    var started: (stats: PresentStats, agent: ProcessUsage?, dext: ProcessUsage?, at: UInt64)?

    func report() {
        guard let started, let mode = mirror.mode else {
            print("display-agent: nothing measured (the run ended within --warmup)")
            return
        }
        guard let handle = mirror.queue.sync(execute: { mirror.handles.values.first }),
              let end = observer.displayStats(handle: handle) else {
            print("display-agent: the driver's statistics could not be read at the end; nothing reported")
            return
        }
        let seconds = Double(uptimeNs() - started.at) / 1e9
        let measured = mirror.queue.sync { mirror.measurement }
        print(String(format: "display-agent: measured %.1f s of workload %@", seconds, workload.kind))
        for line in measured.lines(start: started.stats, end: end, seconds: seconds, refreshHz: mode.refreshRate) {
            print("display-agent:   " + line)
        }
        let flips = Double(max(end.flipped - started.stats.flipped, 1))
        if let a0 = started.agent, let a1 = ProcessUsage.current() {
            let (cpu, wakeups) = a1.rates(since: a0)
            print(String(format: "display-agent:   agent process: %.2f%% CPU, %.0f wakeups/s, %.1f us CPU per flipped frame",
                         cpu, wakeups, Double(a1.cpuNs - a0.cpuNs) / flips / 1e3))
        }
        if let d0 = started.dext, let pid = dextPID, let d1 = ProcessUsage.sampled(pid: pid) {
            let (cpu, wakeups) = d1.rates(since: d0)
            print(String(format: "display-agent:   driver process %d: %.2f%% CPU, %.0f idle wakeups/s, %.1f us CPU per flipped frame (top: 10 ms CPU resolution)",
                         pid, cpu, wakeups, Double(d1.cpuNs - d0.cpuNs) / flips / 1e3))
        } else {
            print("display-agent:   driver process: not measured (" +
                  (dextPID == nil ? "not found; pass --dext-pid" : "top(1) did not report it") + ")")
        }
        if end.error != 0 { print("display-agent:   the driver's output worker stopped: \(displayErrno(Int64(end.error)))") }
    }

    func teardown() -> Int32 {
        workload.stop()
        if outputOn { report() }
        mirror.stopCapture()
        mirror.releaseImports()
        if outputOn {
            let (kr, status, _) = observer.display(.off)
            print("display-agent: monitor restored: " + (kr == kIOReturnSuccess ? displayErrno(status) : callFailure(kr, "OFF")))
        }
        mirror.display = nil
        print("display-agent: virtual display removed")
        if let failure = mirror.failure {
            print("display-agent: FAILED: \(failure)")
            return 1
        }
        return 0
    }

    guard mirror.createDisplay() else { return .ended(teardown()) }
    print("display-agent: virtual display \(mirror.display!.displayID) \"\(plan.name)\" is online")
    guard let mode = mirror.currentMode() else {
        mirror.failure = "macOS uses a mode for the virtual display that the monitor did not list"
        return .ended(teardown())
    }
    guard mirror.startOutput(mode) else { return .ended(teardown()) }
    outputOn = true
    guard mirror.startCapture(mode) else { return .ended(teardown()) }
    if let error = workload.start(displayID: mirror.display!.displayID, refreshHz: mode.refreshRate) {
        mirror.failure = "workload: \(error)"
        return .ended(teardown())
    }
    print("display-agent: mirroring \(plan.connector) with workload \(workload.kind)" +
          (warmup.isFinite ? String(format: ", measured after %.1f s", warmup) : ""))
    fflush(stdout)

    let begin = Date()
    var lastReport = Date(), lastHotplugCheck = Date()
    var measurementSkipped = false
    while !shouldStop() {
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        if mirror.queue.sync(execute: { mirror.failure }) != nil { break }
        // The measured window opens once a surface is imported (statistics
        // are read through one). A failed read skips the measurement; it
        // never stops the mirroring.
        if started == nil && !measurementSkipped && Date().timeIntervalSince(begin) >= warmup,
           let handle = mirror.queue.sync(execute: { mirror.handles.values.first }) {
            if let stats = observer.displayStats(handle: handle) {
                mirror.queue.sync { mirror.measurement = PresentMeasurement(); mirror.measurement.lastFlipped = stats.flipped }
                started = (stats, ProcessUsage.current(), dextPID.flatMap { ProcessUsage.sampled(pid: $0) }, uptimeNs())
            } else {
                print("display-agent: the driver's statistics could not be read; mirroring continues unmeasured")
                measurementSkipped = true
            }
        }
        // A mode chosen in System Settings › Displays: relight at it.
        if let now = mirror.currentMode(), now != mirror.mode {
            print(String(format: "display-agent: macOS switched to %dx%d @ %.3f Hz; the measurement restarts",
                         now.width, now.height, now.refreshRate))
            mirror.stopCapture()
            mirror.releaseImports()
            mirror.queue.sync { mirror.stopped = false }
            guard mirror.startOutput(now), mirror.startCapture(now) else { break }
            started = nil
        }
        // The daemon follows hotplug: the driver's cached state, read every
        // 2 s (no GPU access); the monitor gone ends this mirroring.
        if daemon && Date().timeIntervalSince(lastHotplugCheck) >= 2 {
            lastHotplugCheck = Date()
            let (kr, _, status) = observer.display(.status)
            if kr != kIOReturnSuccess { mirror.failure = callFailure(kr, "STATUS"); break }
            if let status, !status.connectors.contains(where: { $0.name == plan.connector && $0.connected }) {
                print("display-agent: \(plan.connector) disconnected")
                monitorLeft = true
                break
            }
        }
        if !daemon && Date().timeIntervalSince(lastReport) >= 5 {
            lastReport = Date()
            let (f, p, l) = mirror.queue.sync { (mirror.measurement.frames, mirror.measurement.presented, mirror.last) }
            print("display-agent: \(f) frame(s) captured, \(p) presented" +
                  (l.map { ", \($0.flipped) flipped since OUTPUT" } ?? ""))
        }
    }
    let code = teardown()
    return monitorLeft && code == 0 ? .monitorGone : .ended(code)
}

// ----------------------------------------------------------------
// MARK: - display-agent --create --daemon
// ----------------------------------------------------------------

/// The driver service, followed with IOKit matching notifications on the
/// main run loop: no polling while it is absent.
private final class DriverWatch {
    private(set) var present = false
    private var port: IONotificationPortRef?
    private var matched: io_iterator_t = 0, terminated: io_iterator_t = 0

    init?(bundleIdentifier: String) {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return nil }
        self.port = port
        CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port).takeUnretainedValue(),
                           .defaultMode)
        func matching() -> CFDictionary {
            let dict = IOServiceMatching("IOUserService") as NSMutableDictionary
            dict["IOPropertyMatch"] = ["CFBundleIdentifier": bundleIdentifier]
            return dict
        }
        let context = Unmanaged.passUnretained(self).toOpaque()
        let onMatch: IOServiceMatchingCallback = { context, iterator in
            let watch = Unmanaged<DriverWatch>.fromOpaque(context!).takeUnretainedValue()
            if DriverWatch.drain(iterator) { watch.present = true; agentLog("display-agent: the driver is attached") }
        }
        let onTerminate: IOServiceMatchingCallback = { context, iterator in
            let watch = Unmanaged<DriverWatch>.fromOpaque(context!).takeUnretainedValue()
            if DriverWatch.drain(iterator) {
                watch.present = !DriverInstances.list().isEmpty
                agentLog("display-agent: a driver instance left" + (watch.present ? "; another is attached" : ""))
            }
        }
        guard IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, matching(), onMatch, context,
                                               &matched) == KERN_SUCCESS,
              IOServiceAddMatchingNotification(port, kIOTerminatedNotification, matching(), onTerminate, context,
                                               &terminated) == KERN_SUCCESS else { return nil }
        // Arm both iterators; services already present count as matched.
        present = DriverWatch.drain(matched)
        _ = DriverWatch.drain(terminated)
    }

    /// Consume an iterator (which re-arms its notification); true if it
    /// held a service.
    private static func drain(_ iterator: io_iterator_t) -> Bool {
        var any = false
        while case let service = IOIteratorNext(iterator), service != 0 {
            any = true
            IOObjectRelease(service)
        }
        return any
    }
}

/// display-agent --create --daemon: the per-user LaunchAgent
/// (DisplayAutostart) that makes the GPU's monitor a Mac display whenever
/// the driver runs. It waits for the driver (IOKit matching), brings the
/// GPU up if needed (its own session), mirrors the connected monitor, follows
/// hotplug, tears down when the driver or the monitor leaves, and comes back
/// when they return. SIGTERM (launchctl bootout) ends it cleanly. It never
/// shows in the Dock.
func runDisplayAgentDaemon(_ options: [String]) -> Int32 {
    agentWindowServerSetup()
    agentHandleSignals()
    agentLog("display-agent: daemon started (pid \(getpid()))")
    guard CGPreflightScreenCaptureAccess() else {
        _ = CGRequestScreenCaptureAccess()
        agentLog("display-agent: Screen Recording permission is needed: allow MacLinuxGPUHost in System Settings › Privacy & Security › Screen & System Audio Recording")
        // launchd restarts the agent (not a successful exit) after its throttle.
        return 1
    }
    guard let watch = DriverWatch(bundleIdentifier: "com.geramyloveless.MacAMDGPUHost.MacAMDGPU") else {
        agentLog("display-agent: could not watch for the driver (IOKit notifications)")
        return 1
    }
    var retryAfter: Date?
    while !agentInterrupted {
        if !watch.present {
            agentLog("display-agent: waiting for the driver")
            while !agentInterrupted && !watch.present {
                // Returns when a source fires: the match, or a signal.
                _ = RunLoop.main.run(mode: .default, before: .distantFuture)
            }
            continue
        }
        if let until = retryAfter {
            retryAfter = nil
            while !agentInterrupted && watch.present && Date() < until {
                _ = RunLoop.main.run(mode: .default, before: until)
            }
            continue
        }
        // Our own session, so the GPU is up while a monitor is mirrored;
        // compute clients join the same session alongside it.
        guard let (session, observer) = openClients(["--init"]) else {
            agentLog("display-agent: the GPU could not be brought up; retrying in 10 s")
            retryAfter = Date().addingTimeInterval(10)
            continue
        }
        let end = mirrorMonitor(observer: observer, options: options, daemon: true,
                                shouldStop: { agentInterrupted || !watch.present })
        if case .noMonitor = end {
            // Wait for a monitor: the driver's cached hotplug state, every
            // 2 s (no GPU access). The session stays, so the GPU need not
            // come up again for the monitor.
            agentLog("display-agent: no monitor connected; waiting for one")
            var connected = false
            while !agentInterrupted && watch.present && !connected {
                _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(2))
                let (kr, _, status) = observer.display(.status)
                if kr != kIOReturnSuccess { break }
                connected = status?.connectors.contains { $0.connected } ?? false
            }
            observer.closeUserClient()
            session?.closeUserClient()
            continue
        }
        observer.closeUserClient()
        session?.closeUserClient()
        switch end {
        case .ended(let code) where code != 0 && !agentInterrupted && watch.present:
            agentLog("display-agent: mirroring ended with an error; retrying in 10 s")
            retryAfter = Date().addingTimeInterval(10)
        case .monitorGone:
            agentLog("display-agent: the monitor left; waiting for it")
        default:
            break
        }
    }
    agentLog("display-agent: daemon stopped")
    return 0
}

// ----------------------------------------------------------------
// MARK: - autostart (per-user LaunchAgent)
// ----------------------------------------------------------------

/// The display daemon as a per-user LaunchAgent: it runs at login and
/// whenever it is enabled, in the user's GUI session (the WindowServer is
/// per user, so the driver cannot create Mac displays itself), and logs to
/// ~/Library/Logs/MacLinuxGPU-display.log. The installer enables it once
/// the driver is verified unless the user turned it off; a driver upgrade
/// stops it while the previous driver hands over the GPU.
enum DisplayAutostart {
    static let label = "com.geramyloveless.maclinuxgpu.display-agent"
    private static let declinedKey = "DisplayAutostartDeclined"
    static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }
    static var logURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/MacLinuxGPU-display.log")
    }
    static var isEnabled: Bool { FileManager.default.fileExists(atPath: plistURL.path) }
    static var isRunning: Bool { launchctl(["print", "\(domain)/\(label)"]).status == 0 }
    private static var domain: String { "gui/\(getuid())" }

    static func plist(executable: String) -> [String: Any] {
        [
            "Label": label,
            "ProgramArguments": [executable, "display-agent", "--create", "--daemon"],
            "RunAtLoad": true,
            "KeepAlive": ["SuccessfulExit": false],
            "LimitLoadToSessionType": "Aqua",
            "ProcessType": "Interactive",
            "ThrottleInterval": 10,
            "StandardOutPath": logURL.path,
            "StandardErrorPath": logURL.path,
        ]
    }

    @discardableResult
    static func launchctl(_ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do { try process.run() } catch { return (-1, "\(error)") }
        let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        return (process.terminationStatus, output)
    }

    /// Install and start the agent for the app at @executable (the installed
    /// app). nil on success, else why not.
    static func enable(executable: String = Bundle.main.executablePath ?? "") -> String? {
        UserDefaults.standard.set(false, forKey: declinedKey)
        do {
            try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(fromPropertyList: plist(executable: executable),
                                                          format: .xml, options: 0)
            try data.write(to: plistURL, options: .atomic)
        } catch {
            return "could not write \(plistURL.path): \(error.localizedDescription)"
        }
        launchctl(["bootout", "\(domain)/\(label)"])
        let started = launchctl(["bootstrap", domain, plistURL.path])
        return started.status == 0 ? nil : "launchctl bootstrap failed (\(started.status)): \(started.output)"
    }

    /// Stop the agent and remove it; the installer will not enable it again.
    static func disable() -> String? {
        UserDefaults.standard.set(true, forKey: declinedKey)
        launchctl(["bootout", "\(domain)/\(label)"])
        do {
            if isEnabled { try FileManager.default.removeItem(at: plistURL) }
        } catch {
            return "could not remove \(plistURL.path): \(error.localizedDescription)"
        }
        return nil
    }

    /// What the installer does after the driver is verified: enable unless
    /// the user turned it off.
    static func enableUnlessDeclined() -> String? {
        if UserDefaults.standard.bool(forKey: declinedKey) { return nil }
        return enable()
    }

    /// A driver upgrade: the daemon's session would keep the previous driver
    /// from handing over the GPU. Stopped, and started again afterwards.
    static func suspendForUpgrade() {
        if isEnabled { launchctl(["bootout", "\(domain)/\(label)"]) }
    }
    static func resumeAfterUpgrade() {
        if isEnabled && !isRunning { launchctl(["bootstrap", domain, plistURL.path]) }
    }
}

/// display-autostart on|off|status (the CLI side of the app's toggle).
func runDisplayAutostart(_ options: [String]) -> Int32 {
    switch options.first ?? "status" {
    case "on":
        if let error = DisplayAutostart.enable() { print("display-autostart: \(error)"); return 1 }
        print("display-autostart: on (\(DisplayAutostart.plistURL.path)); log \(DisplayAutostart.logURL.path)")
    case "off":
        if let error = DisplayAutostart.disable() { print("display-autostart: \(error)"); return 1 }
        print("display-autostart: off")
    default:
        print("display-autostart: " + (DisplayAutostart.isEnabled ? "on" : "off") +
              (DisplayAutostart.isRunning ? ", running" : ", not running") +
              "; log \(DisplayAutostart.logURL.path)")
    }
    return 0
}
