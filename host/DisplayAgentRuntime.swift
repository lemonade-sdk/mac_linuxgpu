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
        let (kr, values, _) = callDisplayAsync([DisplayOp.importSurface.rawValue, geometry, kDisplayConfirm],
                                               input: base, inputLength: length, outSize: 0)
        guard kr == kIOReturnSuccess, values.count >= 2 else { return (kr, 0, 0) }
        return (kr, Int64(bitPattern: values[0]), UInt32(truncatingIfNeeded: values[1]))
    }

    func displayVerify(handle: UInt32, seed: UInt32) -> (kern_return_t, Int64, SurfaceVerifyResult?) {
        let (kr, values, data) = callDisplayAsync([DisplayOp.verify.rawValue, UInt64(handle) << 32 | UInt64(seed),
                                                   kDisplayConfirm], input: nil, inputLength: 0,
                                                  outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), SurfaceVerifyResult(data))
    }

    func displayRelease(handle: UInt32) -> (kern_return_t, Int64) {
        let (kr, values, _) = callDisplayAsync([DisplayOp.release.rawValue, UInt64(handle), kDisplayConfirm],
                                               input: nil, inputLength: 0, outSize: 0)
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
        let (kr, values, data) = request.withUnsafeBytes { bytes in
            callDisplayAsync([DisplayOp.output.rawValue, UInt64(refreshMilliHz), kDisplayConfirm],
                             input: bytes.baseAddress, inputLength: request.count, outSize: kDisplayReportMax)
        }
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), DisplayReport(data))
    }

    /// PRESENT: queue a frame (its damage and capture time, mach ns) for the
    /// driver's output worker; returns once queued. No rectangle and handle
    /// 0 reads the worker's statistics only.
    func displayPresent(handle: UInt32, rects: [(x: UInt32, y: UInt32, w: UInt32, h: UInt32)],
                        moves: [DamageFilter.Move] = [], captureNs: UInt64) -> (kern_return_t, Int64, PresentStats?) {
        var request = Data(count: 16 + rects.count * 16 + moves.count * 24)
        request.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: UInt32(rects.count).littleEndian, toByteOffset: 0, as: UInt32.self)
            raw.storeBytes(of: UInt32(moves.count).littleEndian, toByteOffset: 4, as: UInt32.self)
            raw.storeBytes(of: captureNs.littleEndian, toByteOffset: 8, as: UInt64.self)
            for (i, r) in rects.enumerated() {
                for (j, v) in [r.x, r.y, r.w, r.h].enumerated() {
                    raw.storeBytes(of: v.littleEndian, toByteOffset: 16 + i * 16 + j * 4, as: UInt32.self)
                }
            }
            let at = 16 + rects.count * 16
            for (i, m) in moves.enumerated() {
                for (j, v) in [m.x, m.y, m.w, m.h, m.srcY, 0].enumerated() {
                    raw.storeBytes(of: v.littleEndian, toByteOffset: at + i * 24 + j * 4, as: UInt32.self)
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
    /// Presented frames whose buffers ScreenCaptureKit must not reuse yet:
    /// the driver copies after PRESENT returns, so a frame's buffer is held
    /// until the driver has flipped it (its copy is done; the flip waited
    /// for it) or replaced it with a newer frame (never copied).
    var held = PresentedFrames<CMSampleBuffer>()
    /// What the driver was given, by tile: damage that changes nothing is
    /// dropped, rows that scrolled are moved (damageMode).
    var filter: DamageFilter?
    /// Damage of frames PRESENT answered busy for, presented with the next.
    var unpresented: [DamageFilter.Rect] = []
    enum DamageMode { case raw, filtered, scroll }
    var damageMode = DamageMode.scroll
    /// CGVirtualDisplaySettings.refreshDeadline (0: not set; --refresh-deadline)
    /// and the capture's minimum frame interval: none by default, so the
    /// stream wakes right after each composition rather than on a grid of
    /// its own (typing on build 239 with the keep-alive: change to flip
    /// p50 30 -> 21 ms, p90 40 -> 30 ms, max 47 -> 35 ms; --frame-interval
    /// refresh for one refresh). Sidecar's capture sets no minimum frame
    /// time either; its refreshDeadline 0.004 measured no better here.
    var refreshDeadline = 0.0, frameIntervalZero = true
    /// The type workload's timeline, when it runs.
    var typeProbe: TypeProbe?
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
        descriptor.queue = DispatchQueue(label: "MacLinuxGPU.display-agent.virtual-display", qos: .userInteractive)
        descriptor.terminationHandler = { _, _ in print("display-agent: macOS ended the virtual display") }
        guard let display = CGVirtualDisplay(descriptor: descriptor) else {
            failure = "CGVirtualDisplay could not be created"
            return false
        }
        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = UInt32(plan.hiDPI)
        settings.refreshDeadline = refreshDeadline
        settings.modes = plan.modes.map {
            CGVirtualDisplayMode(width: UInt32($0.width), height: UInt32($0.height), refreshRate: $0.refreshRate)
        }
        guard display.apply(settings) else {
            failure = "CGVirtualDisplay rejected the mode list"
            return false
        }
        self.display = display
        // macOS brings the display online, then gives it a mode,
        // asynchronously.
        for _ in 0..<50 {
            var count: UInt32 = 0
            CGGetOnlineDisplayList(0, nil, &count)
            var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
            CGGetOnlineDisplayList(count, &ids, &count)
            if ids.contains(display.displayID) && currentMode() != nil { return true }
            Thread.sleep(forTimeInterval: 0.1)
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
        let outputStart = uptimeNs()
        let (kr, status, _) = observer.displayOutput(connector: plan.connector, width: mode.width,
                                                     height: mode.height, refreshMilliHz: mhz)
        let outputNs = uptimeNs() - outputStart
        guard kr == kIOReturnSuccess, status == 0 else {
            failure = kr == kIOReturnSuccess ? "OUTPUT \(mode.width)x\(mode.height)@\(mhz) mHz: \(displayErrno(status))" :
                                               callFailure(kr, "OUTPUT")
            return false
        }
        self.mode = mode
        print(String(format: "display-agent: %@ lit at %dx%d @ %.3f Hz", plan.connector, mode.width,
                     mode.height, mode.refreshRate))
        print(String(format: "display-agent: OUTPUT (the modeset, three framebuffers) took %.1f ms",
                     Double(outputNs) / 1e6))
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
        configuration.minimumFrameInterval = frameIntervalZero ? .zero :
            CMTime(value: 1000, timescale: CMTimeScale((mode.refreshRate * 1000).rounded()))
        configuration.queueDepth = 6 // frames held until done (see held) and one being captured
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

    /// The type workload: poll the driver (1 ms) until the frame flipped,
    /// then record PRESENT to flip and change to flip from its vblank time.
    private func followFlip(_ probe: TypeProbe, target: UInt64, captureNs: UInt64, returned: UInt64,
                            changes: [UInt64], attempts: Int) {
        queue.asyncAfter(deadline: .now() + .milliseconds(1)) { [self] in
            guard !stopped, failure == nil, let handle = handles.values.first,
                  let stats = observer.displayStats(handle: handle) else { return }
            if stats.flipped >= target {
                // Its own vblank time when it is the last flipped frame; else when seen.
                let flipNs = stats.flipped == target && stats.lastLatencyNs > 0 && captureNs > 0 ?
                    captureNs + stats.lastLatencyNs : uptimeNs()
                if flipNs > returned { probe.flipped.add(flipNs - returned) }
                for change in changes where flipNs > change { probe.total.add(flipNs - change) }
            } else if attempts < 1000 {
                followFlip(probe, target: target, captureNs: captureNs, returned: returned, changes: changes,
                           attempts: attempts + 1)
            }
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
        let handlerStart = threadCPUNs(), handlerWall = uptimeNs()
        // The frame's composition time on the virtual display, and how
        // long after it this handler runs.
        if let display = info[.displayTime] as? UInt64 {
            let composed = machToNs(display)
            measurement.delivered(lagNs: handlerWall > composed ? handlerWall - composed : 0)
        }
        measurement.frames += 1
        let id = IOSurfaceGetID(surface)
        var handle = handles[id]
        if handle == nil {
            let width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface)
            guard width == mode.width, height == mode.height else { return } // a frame of the old mode
            IOSurfaceLock(surface, .readOnly, nil)
            let importStart = uptimeNs()
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
            print(String(format: "display-agent: capture surface %u imported as handle %u (%.1f ms)", id, h,
                         Double(uptimeNs() - importStart) / 1e6))
        }
        let dirty = (info[.dirtyRects] as? [NSDictionary] ?? []).compactMap { CGRect(dictionaryRepresentation: $0) }
        let rects = presentRects(dirty, width: mode.width, height: mode.height)
        if rects.isEmpty { measurement.idleFrames += 1; return }
        // ScreenCaptureKit hands over the buffer while the compositor's GPU
        // may still be writing it: read at delivery, its damaged rows can
        // still hold the previous frame (an old window position the
        // driver's copy would then keep on screen). The lock waits for
        // those writes; nothing writes the buffer again while it is held.
        let lockWall = uptimeNs()
        let lock = IOSurfaceLock(surface, .readOnly, nil)
        let lockWaitNs = uptimeNs() - lockWall
        guard lock == kIOReturnSuccess else {
            failure = "IOSurfaceLock of capture surface \(id): " + String(format: "0x%08x", lock)
            return
        }
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        // The type workload: which change, if any, this frame first shows.
        var typeChanges: [(seq: Int, ns: UInt64)] = []
        if let probe = typeProbe, probe.points.count == 4,
           probe.points.allSatisfy({ $0.x >= 0 && $0.y >= 0 && $0.x < mode.width && $0.y < mode.height }) {
            let base = IOSurfaceGetBaseAddress(surface), pitch = IOSurfaceGetBytesPerRow(surface)
            let brightness = probe.points.map {
                Int((base + $0.y * pitch).load(fromByteOffset: $0.x * 4 + 1, as: UInt8.self))   // BGRA: green
            }
            typeChanges = probe.firstShown(index: typeMarkIndex(brightness: brightness), now: handlerWall)
        }
        if filter?.width != mode.width || filter?.height != mode.height {
            filter = DamageFilter(width: mode.width, height: mode.height)
            filter!.detectScroll = damageMode == .scroll
        }
        let filterWall = uptimeNs()
        let kept = damageMode == .raw ? DamageFilter.Damage(rects: rects, moves: []) :
            filter!.filter(rects, base: UnsafeRawPointer(IOSurfaceGetBaseAddress(surface)),
                           pitch: IOSurfaceGetBytesPerRow(surface))
        measurement.filtered(reported: rects, kept: kept, ns: uptimeNs() - filterWall)
        if kept.isEmpty { return }
        // The frame's composition time on the virtual display.
        let captureNs = (info[.displayTime] as? UInt64).map(machToNs) ?? 0
        let callCPU = threadCPUNs(), callWall = uptimeNs()
        // Damage of frames the driver was busy for comes along (as rectangles).
        var sendRects = kept.rects + unpresented
        var sendMoves = kept.moves
        if 16 + sendRects.count * 16 + sendMoves.count * 24 > DamageFilter.requestMax || sendRects.count > 255 {
            sendRects = [(0, 0, UInt32(mode.width), UInt32(mode.height))]
            sendMoves = []
        }
        let (kr, status, stats) = observer.displayPresent(handle: handle!, rects: sendRects, moves: sendMoves,
                                                          captureNs: captureNs)
        let callWallNs = uptimeNs() - callWall, callCPUNs = threadCPUNs() - callCPU
        if kr == kIOReturnSuccess && status == -16 {
            // EBUSY: an op that can sleep holds the display (a STATUS poll,
            // a probe). Never waited for; this frame's damage goes with the
            // next one, and the filter forgets what it recorded as given
            // (the driver drew none of it, so no later frame may move rows
            // from it or skip them as unchanged).
            unpresented = sendRects + sendMoves.map { ($0.x, $0.y, $0.w, $0.h) }
            filter?.reset()
            measurement.busyFrames += 1
            return
        }
        guard kr == kIOReturnSuccess, status == 0, let stats else {
            failure = kr == kIOReturnSuccess ?
                "PRESENT: \(displayErrno(status))" + (stats.map { " (worker error \($0.error))" } ?? "") :
                callFailure(kr, "PRESENT")
            return
        }
        last = stats
        unpresented = []
        if !typeChanges.isEmpty, let probe = typeProbe {
            let returned = uptimeNs()
            for change in typeChanges where captureNs > change.ns { probe.composed.add(captureNs - change.ns) }
            // The display time can be the frame's target vblank, after the handler runs.
            probe.delivered.add(handlerWall > captureNs ? handlerWall - captureNs : 0)
            probe.presented.add(returned - handlerWall)
            // Taken frames so far, this one included: it flips at that count.
            followFlip(probe, target: stats.received - stats.replaced, captureNs: captureNs, returned: returned,
                       changes: typeChanges.map { $0.ns }, attempts: 0)
        }
        measurement.observe(stats)
        _ = held.presented(sample, received: stats.received, replaced: stats.replaced, flipped: stats.flipped)
        measurement.present(callCPUNs: callCPUNs, callWallNs: callWallNs, handlerCPUNs: threadCPUNs() - handlerStart,
                            lockWaitNs: lockWaitNs)
    }
}

/// --workload: what the virtual display shows while it is measured.
///   still  nothing moves (the desktop as it is)
///   move   a 480x320 window crosses the display, moved every refresh
///   full   a window covering the display changes colour every refresh
///          (the damage of full-screen video)
///   scroll a 1600x1100 window scrolls a text document 6 points every
///          refresh under a header that stays
///   type   a glyph-sized mark in a small window changes 5 to 15 times a
///          second at random; each change is followed to its flip
/// Keeps the WindowServer updating the virtual display every refresh.
/// A virtual display has no vsync of its own: once nothing on it changes,
/// the WindowServer lets its update cycle idle, and the next change (a
/// typed character) waits up to ~200 ms to be composited and captured
/// (measured on build 239 with a probe in another process: p90 157 ms,
/// max 203 ms on the virtual display; p90 22 ms on the built-in one).
/// A 2x2 point window in a corner changes its opacity every refresh
/// between two values that composite to the same pixels: the WindowServer
/// composes the display each refresh (p90 26 ms, max 33 ms), and the agent
/// drops the frames it captures as unchanged, so nothing is copied or
/// flipped for it.
final class DisplayKeepAlive {
    private var window: NSWindow?
    private var timer: Timer?
    private var layer: CALayer?
    private var on = false

    /// @displayID's bottom right corner, every 1/@refreshHz s.
    func start(displayID: CGDirectDisplayID, refreshHz: Double) {
        let bounds = CGDisplayBounds(displayID), main = CGDisplayBounds(CGMainDisplayID())
        guard bounds.width > 0 else { return }
        let frame = NSRect(x: bounds.maxX - 2, y: main.height - bounds.maxY, width: 2, height: 2)
        let window = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.level = .screenSaver
        window.ignoresMouseEvents = true
        window.hasShadow = false
        window.isOpaque = false
        window.backgroundColor = .clear
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
        let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
        view.wantsLayer = true
        let layer = CALayer()
        layer.frame = view.bounds
        view.layer?.addSublayer(layer)
        window.contentView = view
        window.setFrame(frame, display: true)
        window.orderFrontRegardless()
        self.window = window
        self.layer = layer
        let timer = Timer(timeInterval: 1 / max(refreshHz, 1), repeats: true) { [weak self] _ in self?.tick() }
        timer.tolerance = 0
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    private func tick() {
        on.toggle()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        // 0.001 and 0.002 of white over anything round to the same 8-bit pixel.
        layer?.backgroundColor = CGColor(gray: 1, alpha: on ? 0.001 : 0.002)
        CATransaction.commit()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        window?.orderOut(nil)
        window = nil
        layer = nil
    }
}

/// The type workload's timeline: each change of the mark (a glyph-sized
/// square) with when the app flushed it to the WindowServer, matched to the
/// first captured frame that shows it (by the mark's grey), then followed
/// to PRESENT and to its flip. Stages: change to the frame's composition
/// (its display time), composition to the frame handler, handler to
/// PRESENT returning, PRESENT to the flip (the driver's vblank time).
final class TypeProbe {
    private let lock = NSLock()
    private var changes: [(seq: Int, ns: UInt64)] = []
    private var seen = Set<Int>()
    /// The marks' centres in the capture (pixels, top-left origin).
    var points: [(x: Int, y: Int)] = []
    var composed = LatencySeries(), delivered = LatencySeries(), presented = LatencySeries()
    var flipped = LatencySeries(), total = LatencySeries()
    var changeCount: Int { lock.lock(); defer { lock.unlock() }; return changes.count }
    var seenCount: Int { lock.lock(); defer { lock.unlock() }; return seen.count }

    func changed(_ seq: Int, at ns: UInt64) {
        lock.lock(); changes.append((seq, ns)); lock.unlock()
    }

    /// The changes a frame showing grey index @index at @now shows first:
    /// the newest change with that index not seen yet, made before @now,
    /// and every earlier change no frame showed (overtaken: this frame is
    /// the first to show the screen past them). Empty when nothing new.
    func firstShown(index: Int, now: UInt64) -> [(seq: Int, ns: UInt64)] {
        lock.lock(); defer { lock.unlock() }
        guard let c = changes.last(where: { $0.seq % 16 == index && $0.ns <= now }), !seen.contains(c.seq)
        else { return [] }
        let shown = changes.filter { $0.seq <= c.seq && !seen.contains($0.seq) }
        for older in shown { seen.insert(older.seq) }
        overtaken += shown.count - 1
        return shown
    }
    private(set) var overtaken = 0

    /// Times are reset with the measured window.
    func reset() {
        lock.lock(); defer { lock.unlock() }
        changes.removeAll(); seen.removeAll(); overtaken = 0
        composed = LatencySeries(); delivered = LatencySeries(); presented = LatencySeries()
        flipped = LatencySeries(); total = LatencySeries()
    }

    var lines: [String] {
        let shown = seenCount, made = changeCount
        return ["typing: \(made) change(s), \(shown) shown (\(overtaken) only by a later change's frame)",
                "  change to composition: " + composed.summary,
                "  composition to frame handler: " + delivered.summary,
                "  frame handler to PRESENT returned: " + presented.summary,
                "  PRESENT to flip: " + flipped.summary,
                "  change to flip: " + total.summary]
    }
}

private final class Workload {
    let kind: String
    var window: NSWindow?
    var timer: Timer?
    var tick = 0
    var position = CGPoint(x: 0, y: 0), velocity = CGPoint(x: 9, y: 6)
    var scrollView: NSScrollView?
    /// type: the mark, the probe, the changes made.
    var typeMarks: [CALayer] = []
    var typeProbe: TypeProbe?
    var typeSeq = 0

    init(kind: String) { self.kind = kind }

    /// type: the next change in 66 to 200 ms (5 to 15 a second, like typing).
    private func typeNext() {
        let delay = Double.random(in: 0.066...0.2)
        let timer = Timer(timeInterval: delay, repeats: false) { [weak self] _ in self?.typeChange() }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    private func typeChange() {
        guard typeMarks.count == 4, let typeProbe, window != nil else { return }
        typeSeq += 1
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for (i, mark) in typeMarks.enumerated() {
            mark.backgroundColor = CGColor(gray: typeMarkBit(typeSeq, i) ? 1 : 0, alpha: 1)
        }
        CATransaction.commit()
        CATransaction.flush()   // to the WindowServer now, as an app's change after a key press
        typeProbe.changed(typeSeq, at: clock_gettime_nsec_np(CLOCK_UPTIME_RAW))
        typeNext()
    }

    /// nil when started; otherwise why not.
    func start(displayID: CGDirectDisplayID, refreshHz: Double) -> String? {
        if kind == "still" { return nil }
        guard ["move", "full", "scroll", "type"].contains(kind) else {
            return "unknown workload \(kind) (still, move, full, scroll or type)"
        }
        // The app is already a background agent (runDisplayAgent).
        // CoreGraphics' global space has its origin at the top left of the
        // main display, AppKit's at the bottom left.
        let bounds = CGDisplayBounds(displayID), main = CGDisplayBounds(CGMainDisplayID())
        guard bounds.width > 0 else { return "the virtual display has no bounds" }
        let screen = NSRect(x: bounds.minX, y: main.height - bounds.maxY, width: bounds.width, height: bounds.height)
        let frame = kind == "full" ? screen :
            kind == "type" ? NSRect(x: screen.minX + 300, y: screen.minY + 300, width: 320, height: 60) :
            kind == "scroll" ? NSRect(x: screen.minX + 200, y: screen.minY + 100, width: min(1600, screen.width - 400),
                                      height: min(1100, screen.height - 200)) :
            NSRect(x: screen.minX, y: screen.minY, width: 480, height: 320)
        let window = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.level = .floating
        window.ignoresMouseEvents = true
        if kind == "type", let probe = typeProbe {
            // A text-field-sized window; the mark is a glyph-sized square in it.
            let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
            view.wantsLayer = true
            view.layer?.backgroundColor = NSColor.textBackgroundColor.cgColor
            typeMarks = (0..<4).map { i in
                let mark = CALayer()
                mark.frame = CGRect(x: 20 + 20 * i, y: 20, width: 12, height: 18)
                mark.backgroundColor = CGColor(gray: 0, alpha: 1)
                view.layer?.addSublayer(mark)
                return mark
            }
            window.contentView = view
            // Capture pixels, top-left origin: each mark's centre (26 + 20 i, 29)
            // up from the window's bottom left.
            probe.points = (0..<4).map { i in
                (Int(frame.minX - screen.minX) + 26 + 20 * i, Int(screen.maxY - (frame.minY + 29)))
            }
        } else if kind == "scroll" {
            // A text document in a scroll view: a header bar that stays, lines that move.
            let content = NSView(frame: NSRect(origin: .zero, size: frame.size))
            let header = NSTextField(labelWithString: "MacLinuxGPU scroll workload")
            header.frame = NSRect(x: 0, y: frame.height - 48, width: frame.width, height: 48)
            header.font = .boldSystemFont(ofSize: 24)
            header.drawsBackground = true
            header.backgroundColor = .controlAccentColor
            let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: frame.width, height: frame.height - 48))
            let text = NSTextView(frame: scroll.bounds)
            text.isEditable = false
            text.font = .monospacedSystemFont(ofSize: 15, weight: .regular)
            text.string = (0..<4000).map { String(format: "%05d  the quick brown fox jumps over the lazy dog %08x %@", $0,
                                                  UInt32(truncatingIfNeeded: $0 &* 2654435761),
                                                  String(repeating: "=", count: $0 % 60)) }.joined(separator: "\n")
            scroll.documentView = text
            scroll.hasVerticalScroller = true
            content.addSubview(scroll)
            content.addSubview(header)
            window.contentView = content
            scrollView = scroll
        } else {
            let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
            view.wantsLayer = true
            view.layer?.backgroundColor = NSColor.systemOrange.cgColor
            window.contentView = view
        }
        window.setFrame(frame, display: true)
        window.orderFrontRegardless()
        self.window = window
        if kind == "type" {
            typeNext()
            return nil
        }
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
        } else if kind == "scroll", let scrollView, let doc = scrollView.documentView {
            // 6 points a refresh, back to the top at the end.
            let clip = scrollView.contentView
            var y = clip.bounds.origin.y + 6
            if y > doc.frame.height - clip.bounds.height { y = 0 }
            clip.scroll(to: NSPoint(x: 0, y: y))
            scrollView.reflectScrolledClipView(clip)
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
/// The agent talks to the WindowServer from the app's own executable, so it
/// becomes a background agent (never in the Dock, no bouncing icon), but
/// only once its virtual display is up and captured: on macOS 26 a process
/// that has finished launching as an NSApplication, or that registered a
/// display reconfiguration callback, before it creates a virtual display
/// never sees that display's modes (CGDisplayCopyDisplayMode stays nil;
/// measured on build 236). A process creates one virtual display, so the
/// daemon runs each mirroring in a child process (runDisplayAgentDaemon).
private var agentIsBackground = false
private func agentBecomeBackground() {
    guard !agentIsBackground else { return }
    agentIsBackground = true
    NSApplication.shared.setActivationPolicy(.accessory)
    NSApplication.shared.finishLaunching()
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

/// Held while a monitor is mirrored: the agent is a background process the
/// system would App Nap (timers coalesced, low QoS), and the virtual display
/// it owns is only as prompt as it is.
private var agentLatencyActivity: NSObjectProtocol?

func runDisplayAgentCreate(_ options: [String]) -> Int32 {
    if options.contains("--daemon") { return runDisplayAgentDaemon(options) }
    // The keep-alive's timer and the frame handler run on time: no App Nap
    // timer coalescing for a process that mirrors a display.
    if agentLatencyActivity == nil {
        agentLatencyActivity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiated, .latencyCritical], reason: "mirroring a monitor as a macOS display")
    }
    let seconds = Double(option(options, "--seconds") ?? "") ?? 0
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
    // --follow-hotplug (the daemon's child): end when the monitor leaves,
    // exit 3 when none is connected, 0 when it left.
    let follow = options.contains("--follow-hotplug")
    if follow { setvbuf(stdout, nil, _IOLBF, 0) }	// the daemon's log, line by line
    switch mirrorMonitor(observer: observer, options: options, daemon: follow,
                         shouldStop: { agentInterrupted || Date() >= deadline }) {
    case .ended(let code): return code
    case .noMonitor: return follow ? 3 : 1
    case .monitorGone: return follow ? 0 : 1
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
    let edid: Data?
    switch observer.readConnectorEDID(connector.name) {
    case .edid(let data): edid = data
    case .unreadable: edid = nil
    case .overran(let kr):
        print("display-agent: \(connector.name): " + callFailure(kr, "EDID read (bounded, overran)"))
        return .ended(1)
    }
    switch planVirtualDisplay(connector: connector.name, modes: modes, edid: edid) {
    case .success(let p): plan = p
    case .failure(let error): print("display-agent: \(connector.name): \(error)"); return .ended(1)
    }
    if !daemon { plan.lines.forEach { print($0) } }

    let mirror = MirroredDisplay(observer: observer, plan: plan)
    // --damage, for measuring: raw (ScreenCaptureKit's rectangles as they
    // come), filtered (unchanged tiles dropped, no moves: a driver before
    // 240 refuses moves) or scroll (the default: filtered, and moves).
    switch daemon ? "scroll" : option(options, "--damage") ?? "scroll" {
    case "raw": mirror.damageMode = .raw
    case "filtered": mirror.damageMode = .filtered
    case "scroll": mirror.damageMode = .scroll
    case let other:
        print("display-agent: unknown --damage \(other) (raw, filtered or scroll)")
        return .ended(1)
    }
    if !daemon {
        // Tuning: CGVirtualDisplaySettings' refreshDeadline (unset by
        // default) and the capture's minimum frame interval (none by default).
        if let text = option(options, "--refresh-deadline") {
            guard let seconds = Double(text), seconds >= 0, seconds < 1 else {
                print("display-agent: --refresh-deadline \(text): seconds from 0 to 1")
                return .ended(1)
            }
            mirror.refreshDeadline = seconds
        }
        switch option(options, "--frame-interval") ?? "zero" {
        case "refresh": mirror.frameIntervalZero = false
        case "zero": mirror.frameIntervalZero = true
        case let other:
            print("display-agent: unknown --frame-interval \(other) (refresh or zero)")
            return .ended(1)
        }
    }
    let keepAlive = DisplayKeepAlive()
    // --keep-alive off, for measuring: the WindowServer idles the display.
    let keepAliveOn = daemon || option(options, "--keep-alive") != "off"
    let workload = Workload(kind: daemon ? "still" : option(options, "--workload") ?? "still")
    if workload.kind == "type" {
        let probe = TypeProbe()
        workload.typeProbe = probe
        mirror.typeProbe = probe
    }
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
        if let probe = mirror.typeProbe {
            mirror.queue.sync {}   // flips being followed land first
            Thread.sleep(forTimeInterval: 0.1)
            mirror.queue.sync { probe.lines.forEach { print("display-agent:   " + $0) } }
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
        keepAlive.stop()
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
    agentBecomeBackground()
    if keepAliveOn { keepAlive.start(displayID: mirror.display!.displayID, refreshHz: mode.refreshRate) }
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
                mirror.typeProbe?.reset()
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

/// The driver trail (TrailFile, DisplayAgent.swift): once a second, on its
/// own queue, the driver instances present, and over its own observer
/// client the driver's cached state and the new bytes of its log ring,
/// appended to ~/Library/Logs/MacLinuxGPU-driver-trail.log and fsynced, so
/// the last second before a panic is on disk. Observer reads are cached
/// and never claim PCI. After "Disconnect GPU" it holds no client, as the
/// rest of the agent does, and records only instances coming and going.
private final class DriverTrail {
    private let queue = DispatchQueue(label: "MacLinuxGPU.display-agent.driver-trail", qos: .utility)
    private var timer: DispatchSourceTimer?
    private let file = TrailFile(url: TrailFile.defaultURL, maxBytes: 4 << 20)
    private var known: [UInt64: String] = [:]          // registry ID -> label
    private var host: MacLinuxGPUHost?
    private var attached: UInt64?
    private var cursor: UInt64 = 0
    private var buffer = TrailLineBuffer()
    private var state: TrailState?
    private var paused = false
    private var lastHeartbeat = Date.distantPast
    private var lastFileError: String?
    private var lastOpenFailure: String?
    private let stamp: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    func start() {
        queue.async { [self] in
            write(["---- display agent \(getpid()) started the driver trail"])
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: .seconds(1), leeway: .milliseconds(100))
            timer.setEventHandler { [weak self] in self?.tick() }
            self.timer = timer
            timer.resume()
        }
    }

    func stop() {
        queue.sync {
            timer?.cancel(); timer = nil
            detach("the display agent is stopping")
            write(["---- display agent \(getpid()) stopped the driver trail"])
        }
    }

    private func write(_ lines: [String]) {
        let now = stamp.string(from: Date())
        let error = file.append(lines.map { "\(now) \($0)" })
        if error != lastFileError {
            lastFileError = error
            if let error { agentLog("display-agent: driver trail: \(error)") }
        }
    }

    private func detach(_ why: String) {
        guard let host else { return }
        var lines: [String] = []
        if let rest = buffer.reset() { lines.append("log: \(rest)") }
        _ = host.closeUserClient()
        self.host = nil
        lines.append(String(format: "trail: detached from instance %#llx (%@) at ring byte %llu",
                            attached ?? 0, why, cursor))
        attached = nil
        state = nil
        write(lines)
    }

    private func tick() {
        var lines: [String] = []
        // Instances coming and going: the line that says the driver died.
        let instances = DriverInstances.list()
        let present = Dictionary(instances.map { ($0.registryID, $0.label) }, uniquingKeysWith: { a, _ in a })
        for (id, label) in known where present[id] == nil {
            var line = "INSTANCE GONE: \(label) left the registry (its process ended or IOKit terminated it)"
            if id == attached, let state { line += "; last state: \(state.summary)" }
            if id == attached { line += String(format: "; last ring byte %llu", cursor) }
            lines.append(line)
        }
        for instance in instances where known[instance.registryID] == nil {
            lines.append("instance appeared: \(instance.label), clients \(instance.clients)")
        }
        known = present
        if !lines.isEmpty { write(lines); lines = [] }
        if let attached, present[attached] == nil { detach("the instance is gone") }

        // After "Disconnect GPU", no client of the driver at all.
        let disconnected = DisplayPrefs.load(from: DisplayControl.prefsURL).disconnected
        if disconnected != paused {
            paused = disconnected
            if disconnected { detach("Disconnect GPU") }
            write([disconnected ? "trail: paused (Disconnect GPU): instances only"
                                : "trail: resumed (the GPU is connected)"])
        }
        if paused { return }

        if host == nil {
            let bundled = DriverInstances.bundledCDHash()
            guard let pick = instances.first(where: { $0.cdhash == bundled }) ?? instances.first else { return }
            let h = MacLinuxGPUHost()
            h.quiet = true
            guard h.openUserClient(observer: true, registryID: pick.registryID) else {
                let failure = "trail: observer open failed for \(pick.label): \(h.log.last ?? "?")"
                if failure != lastOpenFailure { write([failure]) }
                lastOpenFailure = failure
                return
            }
            lastOpenFailure = nil
            host = h
            attached = pick.registryID
            cursor = 0
            buffer = TrailLineBuffer()
            write(["trail: attached to \(pick.label)"])
        }
        guard let host else { return }

        func words(_ tag: UInt64, _ count: Int) -> [UInt64]? {
            let (kr, values) = host.callScalar(21, inScalars: [tag], outScalars: count)
            return kr == kIOReturnSuccess && values.count == count ? values : nil
        }
        let now = TrailState(session: words(0x4c534553, 9), probe: words(0x4c50524f, 5),
                             power: words(0x4c505752, 12), reset: words(0x4c525354, 6))
        if now != state {
            lines.append("state: \(now.summary)")
            state = now
        }
        let result = TrailLogReader.read(from: cursor) { at in
            let (kr, values) = host.callScalar(21, inScalars: [TrailLogReader.tag, at], outScalars: 16)
            return (kr, values)
        }
        switch result {
        case .success(let (data, next, dropped)):
            if dropped > 0 { lines.append("log: \(dropped) byte(s) overwritten in the ring before they were read") }
            lines += buffer.lines(data).map { "log: \($0)" }
            cursor = next
        case .failure(let failure):
            lines.append("trail: log read failed (\(failure)); the instance is \(present[attached ?? 0] != nil ? "still listed" : "gone")")
            write(lines)
            detach("log read failed")
            return
        }
        if Date().timeIntervalSince(lastHeartbeat) >= 30 {
            lastHeartbeat = Date()
            lines.append(String(format: "alive: instance %#llx, ring byte %llu", attached ?? 0, cursor))
        }
        write(lines)
    }
}

/// One monitor's mirroring process, as the daemon follows it.
private final class MirrorChild {
    let connector: String
    let process = Process()
    var exited = false
    var state: DisplayStatus.State = .starting
    var mode: String?
    var lastError: String?
    var buffer = ""
    init(connector: String) { self.connector = connector }
}

/// display-agent --create --daemon: the per-user LaunchAgent
/// (DisplayAutostart) that makes each monitor on the GPU a Mac display
/// whenever the driver runs.
///
/// It waits for the driver (IOKit matching notifications), holds a session
/// so the GPU is up, and reads the monitors from the driver's cached
/// hotplug state every 2 s (a probe when the hotplug epoch moves). Each
/// connected monitor the user has not turned off (DisplayPrefs, by EDID
/// identity) is mirrored by its own child, `display-agent --create --init
/// --follow-hotplug --connector C`: a process sees the modes of one
/// virtual display only (agentBecomeBackground). Turning a monitor off
/// stops its child, which removes its virtual display and restores the
/// monitor. The children's output goes to the log; what they report
/// (mirroring at a mode, or their error) is published for the menu bar in
/// DisplayStatus, with a Darwin notification. The menu bar's changes to
/// DisplayPrefs arrive the same way. SIGTERM stops the children and the
/// daemon. The daemon itself never talks to the WindowServer.
func runDisplayAgentDaemon(_ options: [String]) -> Int32 {
    agentHandleSignals()
    setvbuf(stdout, nil, _IOLBF, 0)
    agentLog("display-agent: daemon started (pid \(getpid()))")
    guard let watch = DriverWatch(bundleIdentifier: "com.geramyloveless.MacAMDGPUHost.MacAMDGPU") else {
        agentLog("display-agent: could not watch for the driver (IOKit notifications)")
        return 1
    }
    // What the driver was doing, on disk every second (survives a panic).
    let trail = DriverTrail()
    trail.start()
    defer { trail.stop() }
    guard let executable = Bundle.main.executablePath else {
        agentLog("display-agent: the app's executable path is unknown")
        return 1
    }
    var prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
    var prefsToken: Int32 = 0
    // The performance choices are written again whenever the GPU comes up
    // (a probe, a power cycle, an upgrade, a reset): reapplyPerformance.
    var performanceApplied = false
    var performanceError: String?
    notify_register_dispatch(DisplayControl.prefsChanged, &prefsToken, DispatchQueue.main) { _ in
        let previous = prefs
        prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
        if prefs.performanceLevel != previous.performanceLevel || prefs.powerProfile != previous.powerProfile {
            // The menu or the CLI set it already; a failure it reported is
            // its own, so this one starts clean.
            performanceError = nil
        }
        agentLog("display-agent: display choices changed")
    }
    defer { notify_cancel(prefsToken) }

    var children: [String: MirrorChild] = [:]
    var retryAt: [String: Date] = [:]
    var lastErrors: [String: String] = [:]
    var monitors: [(connector: String, key: String, name: String)] = []
    var published: DisplayStatus?
    var session: MacLinuxGPUHost?, observer: MacLinuxGPUHost?
    var epoch: UInt32 = .max
    // GPU recovery (LRST): the state last read from this driver instance,
    // and whether the GPU is wedged (until it leaves the bus).
    var lastReset: ResetState?
    var wedged = false
    // Until then a mirror that ends with an error starts again at once:
    // it most likely ended in the reset just seen.
    var restartNowUntil = Date.distantPast

    func publish() {
        var status = DisplayStatus(daemon: getpid(), driverAttached: watch.present, monitors: [],
                                   disconnected: prefs.disconnected && session == nil && observer == nil &&
                                       children.isEmpty, gpuWedged: wedged, performanceError: performanceError)
        for m in monitors {
            let child = children[m.connector]
            let on = prefs.isOn(m.key)
            var state: DisplayStatus.State = .off
            var mode: String?, error: String?
            if let child {
                state = child.state; mode = child.mode; error = child.lastError
            } else if on, let last = lastErrors[m.connector] {
                state = .error; error = last
            } else if on {
                state = .starting
            }
            status.monitors.append(.init(key: m.key, connector: m.connector, name: m.name, on: on,
                                         state: state, mode: mode, error: error))
        }
        guard status != published else { return }
        published = status
        do { try status.save(to: DisplayControl.statusURL) }
        catch { agentLog("display-agent: could not write \(DisplayControl.statusURL.path): \(error)") }
        notify_post(DisplayControl.statusChanged)
    }

    func stop(_ child: MirrorChild) {
        if !child.exited { child.process.terminate() }
        let deadline = Date().addingTimeInterval(15)
        while !child.exited && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.2))
        }
    }

    func start(_ connector: String) {
        let child = MirrorChild(connector: connector)
        child.process.executableURL = URL(fileURLWithPath: executable)
        child.process.arguments = ["display-agent", "--create", "--init", "--follow-hotplug",
                                   "--warmup", "1000000000", "--connector", connector]
        let pipe = Pipe()
        child.process.standardOutput = pipe
        child.process.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            DispatchQueue.main.async {
                guard !data.isEmpty else { return }
                FileHandle.standardOutput.write(data)
                child.buffer += String(decoding: data, as: UTF8.self)
                while let newline = child.buffer.firstIndex(of: "\n") {
                    let line = String(child.buffer[..<newline])
                    child.buffer.removeSubrange(...newline)
                    switch parseAgentLine(line) {
                    case .mirroring(let mode): child.state = .mirroring; child.mode = mode; child.lastError = nil
                    case .failed(let error): child.lastError = error
                    case .other: break
                    }
                }
                publish()
            }
        }
        child.process.terminationHandler = { process in
            DispatchQueue.main.async {
                pipe.fileHandleForReading.readabilityHandler = nil
                child.exited = true
                // 0: the monitor left or it was stopped; 3: no monitor.
                if process.terminationStatus != 0 && process.terminationStatus != 3 {
                    child.state = .error
                    if child.lastError == nil { child.lastError = "the mirroring process ended with status \(process.terminationStatus)" }
                }
            }
        }
        do {
            try child.process.run()
        } catch {
            lastErrors[connector] = "could not start the mirroring process: \(error)"
            retryAt[connector] = Date().addingTimeInterval(10)
            return
        }
        children[connector] = child
        agentLog("display-agent: \(connector): mirroring process \(child.process.processIdentifier) started")
    }

    func closeClients() {
        _ = observer?.closeUserClient(); observer = nil
        _ = session?.closeUserClient(); session = nil
        epoch = .max
        monitors = []
        lastReset = nil
        performanceApplied = false
    }

    func reapplyPerformance(_ why: String) {
        guard let observer, prefs.performanceLevel != nil || prefs.powerProfile != nil else {
            performanceApplied = true; performanceError = nil; return
        }
        performanceError = PerformanceControl.reapply(prefs, host: observer)
        performanceApplied = true
        agentLog("display-agent: performance settings (\(why)): " +
                 (performanceError.map { "not applied: \($0)" } ??
                  "level \(prefs.performanceLevel?.title ?? "unchanged"), profile \(prefs.powerProfile ?? "unchanged")"))
    }

    while !agentInterrupted {
        if !watch.present {
            for child in children.values { stop(child) }
            children = [:]
            closeClients()
            wedged = false      // power-cycled: a new instance comes with the GPU
            if prefs.disconnected {
                // Unplugged after "Disconnect GPU": the next GPU is connected.
                prefs.disconnected = false
                do { try prefs.save(to: DisplayControl.prefsURL) }
                catch { agentLog("display-agent: could not write \(DisplayControl.prefsURL.path): \(error)") }
                notify_post(DisplayControl.prefsChanged)
                agentLog("display-agent: the GPU left after Disconnect GPU; it is reconnected when it returns")
            }
            publish()
            agentLog("display-agent: waiting for the driver")
            while !agentInterrupted && !watch.present {
                _ = RunLoop.main.run(mode: .default, before: .distantFuture)
            }
            continue
        }
        // "Disconnect GPU": no mirroring and no client of the driver, so
        // nothing of ours touches the GPU when it is unplugged.
        if prefs.disconnected {
            for child in children.values { stop(child) }
            children = [:]
            closeClients()
            publish()
            agentLog("display-agent: disconnected from the GPU; it can be unplugged")
            while !agentInterrupted && watch.present && prefs.disconnected {
                _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(1))
            }
            if !agentInterrupted && watch.present { agentLog("display-agent: reconnecting to the GPU") }
            continue
        }
        // A session keeps the GPU up; the observer reads its cached state.
        if session == nil || observer == nil {
            closeClients()
            let s = MacLinuxGPUHost(), o = MacLinuxGPUHost()
            guard s.openUserClient(), s.initDevice(), o.openUserClient(observer: true) else {
                _ = s.closeUserClient(); _ = o.closeUserClient()
                agentLog("display-agent: the GPU could not be brought up; retrying in 10 s")
                let until = Date().addingTimeInterval(10)
                while !agentInterrupted && watch.present && Date() < until {
                    _ = RunLoop.main.run(mode: .default, before: until)
                }
                continue
            }
            session = s; observer = o
        }
        if !performanceApplied { reapplyPerformance("the GPU is up") }
        // GPU recovery, from the cached state at this poll's cadence.
        if let reset = observer!.resetState() {
            switch ResetAction.evaluate(previous: lastReset, current: reset) {
            case .wedged:
                agentLog("display-agent: the GPU is wedged (its reset failed: \(reset.lastResult), generation " +
                         "\(reset.generation)); stopping the mirrors and letting go of the GPU until it is power-cycled")
                wedged = true
                for child in children.values { stop(child) }
                children = [:]
                closeClients()
                if !prefs.disconnected {
                    prefs.disconnected = true
                    do { try prefs.save(to: DisplayControl.prefsURL) }
                    catch { agentLog("display-agent: could not write \(DisplayControl.prefsURL.path): \(error)") }
                    notify_post(DisplayControl.prefsChanged)
                }
                publish()
                continue
            case .remirror:
                agentLog("display-agent: a GPU reset lost VRAM (generation \(reset.generation)); every mirror starts again")
                performanceApplied = false  // the reset reinitialized power management
                for child in children.values { stop(child) }
                children = [:]
                retryAt = [:]
                lastErrors = [:]
                restartNowUntil = Date().addingTimeInterval(15)
            case .restartEnded:
                agentLog("display-agent: GPU queue reset (generation \(reset.generation), \(reset.queueResets) in all); " +
                         "the outputs redraw, and a mirror that ended starts again now")
                retryAt = [:]
                restartNowUntil = Date().addingTimeInterval(15)
            case .none:
                break
            }
            wedged = false
            lastReset = reset
        }
        // The monitors: the cached hotplug state; probe when it moved.
        let (kr, _, report) = observer!.display(.status)
        if kr == kern_return_t(bitPattern: 0xe00002d5) {
            // kIOReturnBusy: a mirroring process's op holds the display; next poll.
            _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(2))
            continue
        }
        guard kr == kIOReturnSuccess, let report else {
            // Never silently: a refused STATUS once looped here reopening
            // the session every pass with nothing in the log.
            let lost = kr == kIOReturnNotAttached || kr == kIOReturnNoDevice || kr == kIOReturnNotOpen ||
                kr == kern_return_t(MACH_SEND_INVALID_DEST)
            agentLog("display-agent: " + (kr == kIOReturnSuccess ? "STATUS: the driver's report is unreadable"
                                                                 : callFailure(kr, "STATUS")) +
                     (lost ? "; reconnecting to the driver" : "; trying again in 10 s"))
            if lost { closeClients() }
            let until = Date().addingTimeInterval(lost ? 2 : 10)
            while !agentInterrupted && watch.present && Date() < until {
                _ = RunLoop.main.run(mode: .default, before: until)
            }
            continue
        }
        if report.hotplugEpoch != epoch {
            let (pkr, _, probed) = observer!.display(.probe)
            if pkr != kIOReturnSuccess {
                agentLog("display-agent: " + callFailure(pkr, "PROBE") + "; using the cached connector state")
            }
            let current = (pkr == kIOReturnSuccess ? probed : nil) ?? report
            // A monitor's key comes from its EDID; a bounded read that
            // overran leaves the epoch unseen, so the next poll reads again
            // rather than keying the monitor by its connector.
            var overrun: kern_return_t?
            let found = current.connectors.filter { $0.connected }.map { c -> (connector: String, key: String, name: String) in
                var edid: Data?
                switch observer!.readConnectorEDID(c.name) {
                case .edid(let data): edid = data
                case .unreadable: break
                case .overran(let kr): overrun = kr
                }
                let summary = edid.flatMap { EDIDSummary($0) }
                return (c.name, monitorKey(edid: edid) ?? "connector-\(c.name)",
                        summary?.name ?? c.name)
            }
            if let overrun {
                agentLog("display-agent: " + callFailure(overrun, "EDID read (bounded, overran)") + "; reading the monitors again")
                _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(2))
                continue
            }
            epoch = report.hotplugEpoch
            monitors = found
            agentLog("display-agent: monitors: " + (monitors.isEmpty ? "none" :
                monitors.map { "\($0.connector) \($0.name) [\($0.key)]" }.joined(separator: ", ")))
        }
        // Reconcile the children with the choices and the monitors.
        let wanted = Set(connectorsToMirror(connected: monitors.map { ($0.connector, $0.key) }, prefs: prefs))
        for (connector, child) in children where child.exited || !wanted.contains(connector) {
            if !child.exited { stop(child) }
            if child.state == .error, let error = child.lastError {
                lastErrors[connector] = error
                retryAt[connector] = Date() < restartNowUntil ? nil : Date().addingTimeInterval(10)
            } else {
                lastErrors[connector] = nil
            }
            children[connector] = nil
            agentLog("display-agent: \(connector): mirroring process ended")
        }
        for connector in wanted where children[connector] == nil {
            if let at = retryAt[connector], at > Date() { continue }
            retryAt[connector] = nil
            start(connector)
        }
        for connector in Array(lastErrors.keys) where !wanted.contains(connector) { lastErrors[connector] = nil }
        publish()
        _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(2))
    }
    for child in children.values { stop(child) }
    children = [:]
    closeClients()
    published = nil
    try? DisplayStatus(daemon: 0, driverAttached: watch.present, monitors: []).save(to: DisplayControl.statusURL)
    notify_post(DisplayControl.statusChanged)
    agentLog("display-agent: daemon stopped")
    return 0
}

// ----------------------------------------------------------------
// MARK: - performance controls (SysfsWrite, selector 89)
// ----------------------------------------------------------------

let kSelSysfsWrite: UInt32 = 89

extension MacLinuxGPUHost {
    /// An allowlisted sysfs write (session_state.h SysfsWrite), run by
    /// upstream's store(): nil when it took, else why not, said in full.
    func sysfsWrite(_ attribute: SysfsWriteAttribute, _ value: String) -> String? {
        let (kr, out, _) = callMethod(kSelSysfsWrite, inScalars: [attribute.rawValue, UInt64(value.utf8.count)],
                                      inData: Data(value.utf8), outScalars: 1, outSize: 0)
        let what = "\(attribute.path) = \(value)"
        switch kr {
        case kIOReturnSuccess:
            guard let result = out.first.map({ Int64(bitPattern: $0) }) else {
                return "\(what): the driver gave no result"
            }
            if result >= 0 { return nil }
            let errno = Int32(clamping: -result)
            return "\(what): the driver refused it (\(String(cString: strerror(errno))), errno \(errno))"
        case kern_return_t(bitPattern: 0xe00002e2):
            return "\(what): not a value the driver allows (or a driver from before build 257)"
        case kern_return_t(bitPattern: 0xe00002d8):
            return "\(what): the GPU is not running in an open session"
        case kern_return_t(bitPattern: 0xe00002d6):
            return "\(what): the driver did not finish within 250 ms; the setting may still take effect"
        default:
            return String(format: "%@: call failed (kr=%#x)", what, kr)
        }
    }
}

/// The performance controls of the menu, the CLI and the daemon's reapply.
enum PerformanceControl {
    private static func text(_ host: MacLinuxGPUHost, _ path: String) -> String? {
        guard let (status, data) = host.sysfsRead(path), status == 0 else { return nil }
        return String(decoding: data, as: UTF8.self)
    }
    static func level(_ host: MacLinuxGPUHost) -> PerformanceLevel? {
        text(host, SysfsWriteAttribute.performanceLevel.path).flatMap { PerformanceLevel(sysfs: $0) }
    }
    /// The level as the file shows it, offered or not ("manual").
    static func levelText(_ host: MacLinuxGPUHost) -> String? {
        text(host, SysfsWriteAttribute.performanceLevel.path)?.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    static func profiles(_ host: MacLinuxGPUHost) -> [PowerProfile] {
        text(host, SysfsWriteAttribute.powerProfile.path).map { PowerProfile.parse($0) } ?? []
    }
    static func reading(_ host: MacLinuxGPUHost) -> PerformanceReading {
        var r = PerformanceReading()
        r.gfxMHz = text(host, "pp_dpm_sclk").flatMap { PerformanceReading.currentMHz($0) }
        r.memoryMHz = text(host, "pp_dpm_mclk").flatMap { PerformanceReading.currentMHz($0) }
        if let hwmon = hwmonDirectory(host) {
            r.temperatureC = text(host, "\(hwmon)/temp1_input").flatMap { Double($0.trimmingCharacters(in: .whitespacesAndNewlines)) }.map { $0 / 1000 }
            let power = text(host, "\(hwmon)/power1_average") ?? text(host, "\(hwmon)/power1_input")
            r.powerW = power.flatMap { Double($0.trimmingCharacters(in: .whitespacesAndNewlines)) }.map { $0 / 1_000_000 }
        }
        return r
    }
    private static func hwmonDirectory(_ host: MacLinuxGPUHost) -> String? {
        guard let (status, data) = host.sysfsRead("hwmon", list: true), status == 0 else { return nil }
        return String(decoding: data, as: UTF8.self).split(separator: "\n")
            .first { $0.hasPrefix("d hwmon") }.map { "hwmon/" + $0.dropFirst(2) }
    }

    /// Set the level now and keep it (the display prefs): nil, or why not.
    static func setLevel(_ level: PerformanceLevel, host: MacLinuxGPUHost) -> String? {
        if let error = host.sysfsWrite(.performanceLevel, level.sysfsValue) { return error }
        if let read = levelText(host), read != level.sysfsValue {
            return "power_dpm_force_performance_level reads \(read) after writing \(level.sysfsValue)"
        }
        return persist { $0.performanceLevel = level }
    }
    /// Set the profile now (by the index the card lists it under) and keep
    /// its name: nil, or why not.
    static func setProfile(_ profile: PowerProfile, host: MacLinuxGPUHost) -> String? {
        if let error = host.sysfsWrite(.powerProfile, String(profile.index)) { return error }
        if let active = profiles(host).first(where: { $0.active }), active.index != profile.index {
            return "pp_power_profile_mode shows \(active.name) active after choosing \(profile.name)"
        }
        return persist { $0.powerProfile = profile.name }
    }
    private static func persist(_ change: (inout DisplayPrefs) -> Void) -> String? {
        var prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
        change(&prefs)
        do { try prefs.save(to: DisplayControl.prefsURL) } catch {
            return "set, but not kept: could not write \(DisplayControl.prefsURL.path): \(error)"
        }
        notify_post(DisplayControl.prefsChanged)
        return nil
    }

    /// The persisted choices, written again (the GPU came up): nil when
    /// every write took (or nothing was chosen), else the first failure.
    static func reapply(_ prefs: DisplayPrefs, host: MacLinuxGPUHost) -> String? {
        switch PerformancePlan.writes(prefs: prefs, profiles: prefs.powerProfile == nil ? [] : profiles(host)) {
        case .failure(let error): return error.message
        case .success(let writes):
            for write in writes { if let error = host.sysfsWrite(write.attribute, write.value) { return error } }
            return nil
        }
    }
}

/// MacLinuxGPUHost performance | performance-level <level> | power-profile <name>
func runPerformanceCommand(_ args: [String]) -> Int32 {
    let host = MacLinuxGPUHost()
    guard host.openUserClient(observer: true) else {
        print("ERROR: failed to open the UserClient (is the driver attached?)")
        return 1
    }
    defer { _ = host.closeUserClient() }
    switch args.first {
    case "performance-level":
        guard args.count == 2, let level = PerformanceLevel(argument: args[1]) else {
            print("usage: MacLinuxGPUHost performance-level auto|high|peak|low"); return 2
        }
        if let error = PerformanceControl.setLevel(level, host: host) { print("ERROR: \(error)"); return 1 }
        print("performance level: \(level.title) (\(level.sysfsValue)); kept, and set again whenever the GPU comes up")
        return 0
    case "power-profile":
        let profiles = PowerProfile.selectable(PerformanceControl.profiles(host))
        guard args.count == 2, let profile = PowerProfile.find(args[1], in: profiles) else {
            print("usage: MacLinuxGPUHost power-profile <name>; the card offers: " +
                  (profiles.isEmpty ? "(none readable)" : profiles.map { "\($0.title) (\($0.name.lowercased()))" }.joined(separator: ", ")))
            return 2
        }
        if let error = PerformanceControl.setProfile(profile, host: host) { print("ERROR: \(error)"); return 1 }
        print("power profile: \(profile.title) (\(profile.name), index \(profile.index)); kept, and set again whenever the GPU comes up")
        return 0
    default:
        let prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
        print("performance level: \(PerformanceControl.levelText(host) ?? "unreadable")" +
              " (kept: \(prefs.performanceLevel?.title ?? "none, the driver's Auto"))")
        let profiles = PerformanceControl.profiles(host)
        print("power profile: \(profiles.first(where: { $0.active }).map { "\($0.title) (\($0.name))" } ?? "unreadable")" +
              " (kept: \(prefs.powerProfile ?? "none"))")
        print(PerformanceControl.reading(host).line)
        return 0
    }
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
        // One agent per monitor: one started by hand would compete for it.
        stopOtherAgents()
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

    /// A driver upgrade: a display agent's session would keep the previous
    /// driver from handing over the GPU. Stopped (the LaunchAgent, and any
    /// agent started by hand), and the LaunchAgent started again afterwards.
    static func suspendForUpgrade() {
        if isEnabled { launchctl(["bootout", "\(domain)/\(label)"]) }
        stopOtherAgents()
    }

    /// Display agents of this user not run by the LaunchAgent (started by
    /// hand, or by an older app): SIGTERM, which restores the monitor and
    /// removes the virtual display; waits up to 15 s for them to go.
    static func stopOtherAgents() {
        let pattern = "MacLinuxGPUHost display-agent --create"
        let list = { () -> [pid_t] in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
            process.arguments = ["-U", String(getuid()), "-f", pattern]
            let pipe = Pipe()
            process.standardOutput = pipe
            guard (try? process.run()) != nil else { return [] }
            let text = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            process.waitUntilExit()
            return text.split(separator: "\n").compactMap { pid_t($0) }.filter { $0 != getpid() }
        }
        let pids = list()
        guard !pids.isEmpty else { return }
        pids.forEach { kill($0, SIGTERM) }
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && !list().isEmpty { Thread.sleep(forTimeInterval: 0.25) }
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

// ----------------------------------------------------------------
// MARK: - menu bar (MacLinuxGPUHost menu-bar)
// ----------------------------------------------------------------

/// The menu bar item: each monitor on the GPU with an On/Off toggle, its
/// live state, the autostart setting, the log and the app. It only reads
/// the daemon's DisplayStatus and writes DisplayPrefs (with a Darwin
/// notification each way); it never creates a virtual display itself. Runs
/// as a background agent (never in the Dock), from its own LaunchAgent
/// (MenuBarAgent).
/// "Disconnect GPU", like ejecting a disk: the display daemon stops
/// mirroring and lets go of the driver; once no app holds the driver and
/// the driver has closed its session, nothing touches the GPU when it is
/// unplugged. "Reconnect GPU" undoes it; unplugging does too (the daemon
/// clears the choice once the GPU has left).
enum GPUDisconnect {
    static func set(disconnected: Bool) -> Bool {
        var prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
        prefs.disconnected = disconnected
        do { try prefs.save(to: DisplayControl.prefsURL) } catch { return false }
        notify_post(DisplayControl.prefsChanged)
        return true
    }

    static var requested: Bool { DisplayPrefs.load(from: DisplayControl.prefsURL).disconnected }

    /// Whether the GPU can be unplugged now (one look, no waiting).
    static func readiness() -> DisconnectReadiness {
        var status = DisplayStatus.load(from: DisplayControl.statusURL) ?? DisplayStatus()
        if status.daemon == 0 || kill(status.daemon, 0) != 0 { status = DisplayStatus() }
        let instances = DriverInstances.list()
        let clients = instances.flatMap { $0.clients }
        var busy = false, open = false
        if !instances.isEmpty {
            let host = MacLinuxGPUHost()
            host.quiet = true
            if host.openUserClient(observer: true) {
                if let session = host.sessionState() {
                    busy = session.closing
                    open = session.pciOpen && !session.closing
                }
                _ = host.closeUserClient()
            }
        }
        return DisconnectReadiness.evaluate(status: status, clients: clients, selfPID: getpid(),
                                            sessionBusy: busy, sessionOpen: open)
    }

    /// The driver closes its session for Disconnect GPU, whatever programs
    /// it has (Retire DISCONNECT): they are told the GPU was disconnected.
    /// False when the driver refused (a raw BAR mapping, a quarantine).
    static func closeSession() -> Bool {
        let host = MacLinuxGPUHost()
        host.quiet = true
        guard host.openUserClient(observer: true) else { return false }
        if let result = host.retire(kRetireOpDisconnect) {
            _ = host.closeUserClient()
            return result.status == kIOReturnSuccess || result.state == RetireResult.closing
        }
        let refused = host.lastStatus
        _ = host.closeUserClient()
        // Not authorized to end other programs' sessions: ShutdownGPU closes
        // the session once none uses it (at once when none does; from build
        // 263 on, when the last of them quits).
        guard refused == kern_return_t(bitPattern: 0xe00002c1) else { return false }
        let session = MacLinuxGPUHost()
        session.quiet = true
        guard session.openUserClient(observer: false) else { return false }
        defer { _ = session.closeUserClient() }
        switch session.shutdownSession() {
        case .closed, .closing: return true
        default: return false
        }
    }

    /// Asks, then waits up to @timeout for the GPU to be free; the last look.
    /// Once the displays let go, a session still up is closed for the
    /// programs using it.
    static func disconnect(timeout: TimeInterval, progress: (DisconnectReadiness) -> Void) -> DisconnectReadiness {
        guard set(disconnected: true) else { return .waitingForDisplays }
        let deadline = Date().addingTimeInterval(timeout)
        var last: DisconnectReadiness?
        var asked = false
        while true {
            let now = readiness()
            if now != last { progress(now); last = now }
            if now.canUnplug || Date() >= deadline { return now }
            if case .appsConnected = now, !asked {
                asked = true
                if !closeSession() { asked = false }
            }
            Thread.sleep(forTimeInterval: 0.25)
        }
    }
}

/// The menu bar's Performance submenu: the level and power profile as the
/// driver shows them now (an observer client while the menu is open), the
/// live clocks, temperature and power refreshed each second while it is
/// open, and a choice written at once (upstream's store()) and kept in the
/// display prefs, which the display daemon writes again whenever the GPU
/// comes up. A refused write is shown here, and beeps.
private final class PerformanceMenu: NSObject, NSMenuDelegate {
    let menu = NSMenu(title: "Performance")
    private var host: MacLinuxGPUHost?
    private var timer: Timer?
    private weak var liveItem: NSMenuItem?
    private var lastError: String?

    override init() {
        super.init()
        menu.delegate = self
        menu.autoenablesItems = false
    }

    private func openHost() -> MacLinuxGPUHost? {
        if host == nil {
            let h = MacLinuxGPUHost()
            h.quiet = true
            if h.openUserClient(observer: true) { host = h }
        }
        return host
    }
    private func closeHost() {
        _ = host?.closeUserClient()
        host = nil
    }
    private func label(_ title: String, indent: Bool = false) -> NSMenuItem {
        let item = NSMenuItem(title: (indent ? "    " : "") + title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        guard let host = openHost() else {
            menu.addItem(label("The driver cannot be reached"))
            return
        }
        let live = label(PerformanceControl.reading(host).line)
        liveItem = live
        menu.addItem(live)
        menu.addItem(.separator())

        menu.addItem(label("Performance level"))
        let current = PerformanceControl.levelText(host)
        for level in PerformanceLevel.allCases {
            let item = NSMenuItem(title: level.title, action: #selector(chooseLevel(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = level.rawValue
            item.state = current == level.sysfsValue ? .on : .off
            menu.addItem(item)
        }
        if let current, PerformanceLevel(sysfs: current) == nil {
            menu.addItem(label("Now: \(current)", indent: true))
        } else if current == nil {
            menu.addItem(label("Unreadable now", indent: true))
        }
        menu.addItem(.separator())

        menu.addItem(label("Power profile"))
        let profiles = PowerProfile.selectable(PerformanceControl.profiles(host))
        if profiles.isEmpty { menu.addItem(label("The card lists no profiles", indent: true)) }
        for profile in profiles {
            let item = NSMenuItem(title: profile.title, action: #selector(chooseProfile(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = profile.index
            item.state = profile.active ? .on : .off
            menu.addItem(item)
        }
        let daemonError = (DisplayStatus.load(from: DisplayControl.statusURL) ?? DisplayStatus()).performanceError
        if let error = lastError ?? daemonError.map({ "not applied when the GPU came up: " + $0 }) {
            menu.addItem(.separator())
            menu.addItem(label("Error: " + error))
        }
        menu.addItem(.separator())

        // Runtime power management modes come here (always up, autosuspend
        // after N seconds, unload when idle).
        menu.addItem(label("Power management"))
        let always = label("Always up", indent: false)
        always.state = .on
        menu.addItem(always)
        menu.addItem(label("Suspend or unload when idle: in a later version", indent: true))
    }

    func menuWillOpen(_ menu: NSMenu) {
        let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
            guard let self, let host = self.host, let live = self.liveItem else { return }
            live.title = PerformanceControl.reading(host).line
        }
        RunLoop.main.add(timer, forMode: .common)
        RunLoop.main.add(timer, forMode: .eventTracking)
        self.timer = timer
    }

    func menuDidClose(_ menu: NSMenu) {
        timer?.invalidate()
        timer = nil
        closeHost()
    }

    @objc func chooseLevel(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String, let level = PerformanceLevel(rawValue: raw),
              let host = openHost() else { NSSound.beep(); return }
        lastError = PerformanceControl.setLevel(level, host: host)
        if lastError != nil { NSSound.beep() }
        closeHost()
    }

    @objc func chooseProfile(_ sender: NSMenuItem) {
        guard let index = sender.representedObject as? Int, let host = openHost(),
              let profile = PowerProfile.selectable(PerformanceControl.profiles(host)).first(where: { $0.index == index })
        else { NSSound.beep(); return }
        lastError = PerformanceControl.setProfile(profile, host: host)
        if lastError != nil { NSSound.beep() }
        closeHost()
    }
}

private final class MenuBarController: NSObject, NSMenuDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    let performance = PerformanceMenu()
    var status = DisplayStatus.load(from: DisplayControl.statusURL) ?? DisplayStatus()
    var token: Int32 = 0

    override init() {
        super.init()
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        notify_register_dispatch(DisplayControl.statusChanged, &token, DispatchQueue.main) { [weak self] _ in
            self?.reload()
        }
        reload()
    }

    func reload() {
        status = DisplayStatus.load(from: DisplayControl.statusURL) ?? DisplayStatus()
        let running = status.daemon != 0 && kill(status.daemon, 0) == 0
        let state: DisplayStatus.State = running ? status.summary : .off
        let symbol = state == .error ? "exclamationmark.triangle" : "display"
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "MacLinuxGPU displays")
        image?.isTemplate = true
        item.button?.image = image
        item.button?.appearsDisabled = state == .off
        item.button?.toolTip = "MacLinuxGPU displays: " + (running ? state.rawValue : "the display agent is not running")
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        reload()
        menu.removeAllItems()
        let running = status.daemon != 0 && kill(status.daemon, 0) == 0
        menu.addItem(withTitle: "MacLinuxGPU Displays", action: nil, keyEquivalent: "").isEnabled = false
        if !running {
            menu.addItem(withTitle: DisplayAutostart.isEnabled ? "The display agent is not running"
                                                               : "Displays are not started automatically",
                         action: nil, keyEquivalent: "").isEnabled = false
        } else if !status.driverAttached {
            menu.addItem(withTitle: "No GPU attached", action: nil, keyEquivalent: "").isEnabled = false
        } else if status.gpuWedged {
            menu.addItem(withTitle: DisplayStatus.wedgedTitle, action: nil, keyEquivalent: "").isEnabled = false
            menu.addItem(withTitle: "    " + DisplayStatus.wedgedAdvice, action: nil, keyEquivalent: "").isEnabled = false
        } else if status.monitors.isEmpty {
            menu.addItem(withTitle: "No monitor connected to the GPU", action: nil, keyEquivalent: "").isEnabled = false
        }
        if running {
            for monitor in status.monitors {
                let toggle = NSMenuItem(title: "\(monitor.name) (\(monitor.connector))",
                                        action: #selector(toggleMonitor(_:)), keyEquivalent: "")
                toggle.target = self
                toggle.state = monitor.on ? .on : .off
                toggle.representedObject = monitor.key
                menu.addItem(toggle)
                let line: String
                switch monitor.state {
                case .off: line = "Off"
                case .starting: line = "Starting…"
                case .mirroring: line = "Mirroring at " + (monitor.mode ?? "?")
                case .error: line = "Error: " + (monitor.error ?? "unknown")
                }
                let detail = NSMenuItem(title: "    " + line, action: nil, keyEquivalent: "")
                detail.isEnabled = false
                menu.addItem(detail)
            }
        }
        menu.addItem(.separator())
        if !GPUDisconnect.requested && !status.gpuWedged {
            let item = NSMenuItem(title: "Performance", action: nil, keyEquivalent: "")
            item.submenu = performance.menu
            menu.addItem(item)
            menu.addItem(.separator())
        }
        if status.driverAttached || GPUDisconnect.requested {
            if GPUDisconnect.requested {
                let reconnect = NSMenuItem(title: "Reconnect GPU", action: #selector(reconnectGPU), keyEquivalent: "")
                reconnect.target = self
                menu.addItem(reconnect)
                let line = NSMenuItem(title: "    " + (status.driverAttached ? GPUDisconnect.readiness().message
                                                                             : "The GPU is unplugged"),
                                      action: nil, keyEquivalent: "")
                line.isEnabled = false
                menu.addItem(line)
            } else {
                let disconnect = NSMenuItem(title: "Disconnect GPU…", action: #selector(disconnectGPU),
                                            keyEquivalent: "")
                disconnect.target = self
                menu.addItem(disconnect)
            }
            menu.addItem(.separator())
        }
        let auto = NSMenuItem(title: "Start displays automatically", action: #selector(toggleAutostart(_:)),
                              keyEquivalent: "")
        auto.target = self
        auto.state = DisplayAutostart.isEnabled ? .on : .off
        menu.addItem(auto)
        let log = NSMenuItem(title: "Open log", action: #selector(openLog), keyEquivalent: "")
        log.target = self
        menu.addItem(log)
        let app = NSMenuItem(title: "Open MacLinuxGPU", action: #selector(openApp), keyEquivalent: "")
        app.target = self
        menu.addItem(app)
    }

    @objc func toggleMonitor(_ sender: NSMenuItem) {
        guard let key = sender.representedObject as? String else { return }
        var prefs = DisplayPrefs.load(from: DisplayControl.prefsURL)
        prefs.set(key, on: !prefs.isOn(key))
        do {
            try prefs.save(to: DisplayControl.prefsURL)
            notify_post(DisplayControl.prefsChanged)
        } catch {
            NSSound.beep()
        }
    }

    @objc func disconnectGPU() {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = GPUDisconnect.disconnect(timeout: 20) { _ in }
            DispatchQueue.main.async { self.showDisconnect(result) }
        }
    }

    private func showDisconnect(_ result: DisconnectReadiness) {
        let alert = NSAlert()
        alert.messageText = result.canUnplug ? "The GPU can be unplugged" : "The GPU is still in use"
        alert.informativeText = result == .safe ?
            "Displays on the GPU are off and nothing is using it. Choose Reconnect GPU in this menu to use it again." :
            result.message
        alert.addButton(withTitle: "OK")
        if !result.canUnplug { alert.addButton(withTitle: "Check Again") }
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertSecondButtonReturn { disconnectGPU() }
    }

    @objc func reconnectGPU() {
        if !GPUDisconnect.set(disconnected: false) { NSSound.beep() }
    }

    @objc func toggleAutostart(_ sender: NSMenuItem) {
        _ = DisplayAutostart.isEnabled ? DisplayAutostart.disable() : DisplayAutostart.enable()
        reload()
    }

    @objc func openLog() {
        NSWorkspace.shared.open(DisplayAutostart.logURL)
    }

    @objc func openApp() {
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        configuration.activates = true
        NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: configuration)
    }
}

func runMenuBar() -> Int32 {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let controller = MenuBarController()
    withExtendedLifetime(controller) { app.run() }
    return 0
}

/// The menu bar item as a per-user LaunchAgent: at login, with the display
/// agent. The installer enables it unless the user hid it in the app.
enum MenuBarAgent {
    static let label = "com.geramyloveless.maclinuxgpu.menu-bar"
    private static let hiddenKey = "MenuBarHidden"
    static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }
    static var isEnabled: Bool { FileManager.default.fileExists(atPath: plistURL.path) }
    private static var domain: String { "gui/\(getuid())" }

    static func enable(executable: String = Bundle.main.executablePath ?? "") -> String? {
        UserDefaults.standard.set(false, forKey: hiddenKey)
        let plist: [String: Any] = [
            "Label": label,
            "ProgramArguments": [executable, "menu-bar"],
            "RunAtLoad": true,
            "KeepAlive": ["SuccessfulExit": false],
            "LimitLoadToSessionType": "Aqua",
            "ProcessType": "Interactive",
        ]
        do {
            try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
                .write(to: plistURL, options: .atomic)
        } catch {
            return "could not write \(plistURL.path): \(error.localizedDescription)"
        }
        DisplayAutostart.launchctl(["bootout", "\(domain)/\(label)"])
        let started = DisplayAutostart.launchctl(["bootstrap", domain, plistURL.path])
        return started.status == 0 ? nil : "launchctl bootstrap failed (\(started.status)): \(started.output)"
    }

    static func disable() -> String? {
        UserDefaults.standard.set(true, forKey: hiddenKey)
        DisplayAutostart.launchctl(["bootout", "\(domain)/\(label)"])
        do { if isEnabled { try FileManager.default.removeItem(at: plistURL) } }
        catch { return "could not remove \(plistURL.path): \(error.localizedDescription)" }
        return nil
    }

    static func enableUnlessHidden() -> String? {
        UserDefaults.standard.bool(forKey: hiddenKey) ? nil : enable()
    }
}
