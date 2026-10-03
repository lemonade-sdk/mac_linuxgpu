//
//  MacLinuxGPUHostApp.swift — the mac_linuxgpu host app (the Xcode build's
//  host-side UserClient driver).
//
//  Ported from mac_amdgpu/Host/MacAMDGPUHostApp.swift (the selector table +
//  the UserClient lifecycle + the firmware push + the status read).  The
//  mac_amdgpu-specific GPU logic is DROPPED (the mac_linuxgpu dext hosts the
//  real upstream KMD, so the host app is thinner — the compute path is the
//  client/ (kfd_client_open/ioctl/create_queue/alloc_vram/dispatch/
//  signal_wait), driven in-process, not over the UserClient selector RPC).
//
//  What this app does:
//    1. Open the UserClient (IOServiceGetMatchingService + IOServiceOpen) —
//       the single-tenant client (type 0).
//    2. Drive the selector-RPC (the kMacAMDGPUMethod* numbers, kept
//       identical to mac_amdgpu so the client ABI is shared).
//    3. Serve firmware: while InitDevice runs, answer the dext's on-demand
//       firmware requests from the installed linux-firmware directory
//       (fw_mailbox_service.c); optionally push single files over the
//       LoadFirmware selector.
//    4. Read status (the bringup state, the error log).
//    5. The compute path (the client/ — kfd_client_open/ioctl/
//       create_queue/alloc_vram/dispatch/signal_wait) is driven in-process
//       (the client links into the dext; the host app's role is the
//       UserClient lifecycle + the firmware push + the status read).
//
//  This app COMPILES host-side (no GPU needed).  The activation (the dext
//  actually loading on the Mac) needs an AMD GPU attached over Thunderbolt
//  and a real signing identity.  No device ID is assumed anywhere: the dext
//  matches AMD display-class functions and upstream amdgpu decides support.
//

import Foundation
import IOKit
import SystemExtensions
import AppKit
import SwiftUI

private let dextBundleIdentifier = "com.geramyloveless.MacAMDGPUHost.MacAMDGPU"
private let hostBundleIdentifier = "com.geramyloveless.MacAMDGPUHost"
private let expectedTeamIdentifier = "YBQ9BU6Q6F"
private let installedAppURL = URL(fileURLWithPath: "/Applications/MacLinuxGPUHost.app", isDirectory: true)
// A /lib/firmware equivalent: request names already carry "amdgpu/".  The
// installer copies the bundled linux-firmware amdgpu directory (with its
// WHENCE and license files) to <root>/amdgpu.
private let firmwareRootPath = "/Library/Application Support/MacLinuxGPU/firmware"

// The disk image contains the signed host app and its embedded dext. Verify the
// exact package before allowing a privileged replacement in /Applications.
private enum InstallPackage {
    // Only the HSA runtime ships with the driver; it must match the dext.
    // HRX and Loom belong to the applications that use them (LemonSeed
    // bundles its own).
    private static let runtimeFiles = ["libhsa-runtime64.0.1.0.dylib"]
    private static let runtimeLinks = [
        "libhsa-runtime64.1.dylib": "libhsa-runtime64.0.1.0.dylib",
        "libhsa-runtime64.dylib": "libhsa-runtime64.1.dylib"
    ]

    private static func itemType(_ url: URL) -> FileAttributeType? {
        (try? FileManager.default.attributesOfItem(atPath: url.path))?[.type] as? FileAttributeType
    }

    static func version(at appURL: URL) -> (short: String, build: String)? {
        let dextURL = appURL.appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext")
        guard let info = NSDictionary(contentsOf: dextURL.appendingPathComponent("Info.plist")),
              let short = info["CFBundleShortVersionString"] as? String,
              let build = info["CFBundleVersion"] as? String else { return nil }
        return (short, build)
    }

    static func validate(_ appURL: URL) -> String? {
        let dextURL = appURL.appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext")
        guard itemType(appURL) == .typeDirectory,
              itemType(dextURL) == .typeDirectory,
              Bundle(url: appURL)?.bundleIdentifier == hostBundleIdentifier,
              Bundle(url: dextURL)?.bundleIdentifier == dextBundleIdentifier,
              version(at: appURL) != nil else {
            return "The app or embedded driver has the wrong bundle identifier or version."
        }
        let verify = run("/usr/bin/codesign", ["--verify", "--deep", "--strict", appURL.path])
        guard verify.status == 0 else { return "Package signature verification failed: \(verify.output)" }
        for bundle in [appURL, dextURL] {
            let identity = run("/usr/bin/codesign", ["-dv", bundle.path])
            guard identity.status == 0,
                  identity.output.split(separator: "\n").contains("TeamIdentifier=\(expectedTeamIdentifier)") else {
                return "The package is not signed by team \(expectedTeamIdentifier)."
            }
        }
        let runtime = appURL.appendingPathComponent("Contents/Resources/Runtime")
        guard itemType(runtime) == .typeDirectory,
              let names = try? FileManager.default.contentsOfDirectory(atPath: runtime.path),
              Set(names) == Set(runtimeFiles).union(runtimeLinks.keys) else {
            return "The bundled GPU runtime is missing or contains unexpected files."
        }
        for name in runtimeFiles {
            let library = runtime.appendingPathComponent(name)
            guard itemType(library) == .typeRegular else {
                return "The bundled runtime library \(name) is missing or is a link."
            }
            let verifyLibrary = run("/usr/bin/codesign", ["--verify", "--strict", library.path])
            let identity = run("/usr/bin/codesign", ["-dv", library.path])
            guard verifyLibrary.status == 0, identity.status == 0,
                  identity.output.split(separator: "\n").contains("TeamIdentifier=\(expectedTeamIdentifier)") else {
                return "The bundled runtime library \(name) has an invalid signature or team."
            }
        }
        for (name, target) in runtimeLinks {
            let link = runtime.appendingPathComponent(name)
            guard itemType(link) == .typeSymbolicLink,
                  (try? FileManager.default.destinationOfSymbolicLink(atPath: link.path)) == target else {
                return "The bundled runtime link \(name) is invalid."
            }
        }
        // The bundled firmware is a whole linux-firmware amdgpu directory,
        // not a per-GPU list; it must carry its provenance file.
        let firmware = appURL.appendingPathComponent("Contents/Resources/firmware")
        if itemType(firmware) != nil {
            guard itemType(firmware) == .typeDirectory,
                  let names = try? FileManager.default.contentsOfDirectory(atPath: firmware.path),
                  names.contains(where: { $0.hasPrefix("WHENCE") }) else {
                return "The bundled firmware directory is not a directory or lacks its WHENCE file."
            }
        }
        return nil
    }

    static func run(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
        } catch {
            return (-1, error.localizedDescription)
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    static func install(_ source: URL) -> (success: Bool, detail: String) {
        let suffix = UUID().uuidString
        let stage = "/Applications/.MacLinuxGPUHost.stage.\(suffix)"
        let backup = "/Applications/.MacLinuxGPUHost.backup.\(suffix)"
        let destination = installedAppURL.path
        let runtimeStage = "/Library/MacAMDGPU/.runtime.stage.\(suffix)"
        let runtimeBackup = "/Library/MacAMDGPU/.runtime.backup.\(suffix)"
        let localStage = "/usr/local/lib/.maclinuxgpu-hsa.stage.\(suffix)"
        let localBackup = "/usr/local/lib/.maclinuxgpu-hsa.backup.\(suffix)"
        let firmwareStage = firmwareRootPath + "/.amdgpu.stage.\(suffix)"
        let firmwareBackup = firmwareRootPath + "/.amdgpu.backup.\(suffix)"
        let command = """
        set -eu
        src=\(shellQuote(source.path))
        stage=\(shellQuote(stage))
        backup=\(shellQuote(backup))
        dst=\(shellQuote(destination))
        runtime_stage=\(shellQuote(runtimeStage))
        runtime_backup=\(shellQuote(runtimeBackup))
        runtime_dst=/Library/MacAMDGPU/runtime
        local_stage=\(shellQuote(localStage))
        local_backup=\(shellQuote(localBackup))
        local_dst=/usr/local/lib
        fw_root=\(shellQuote(firmwareRootPath))
        fw_stage=\(shellQuote(firmwareStage))
        fw_backup=\(shellQuote(firmwareBackup))
        fw_dst="$fw_root/amdgpu"
        fw_old=0
        fw_new=0
        committed=0
        runtime_old=0
        runtime_new=0
        app_old=0
        app_new=0
        local_started=0
        finish() {
          result=$?
          trap - EXIT
          if [ "$committed" -ne 1 ]; then
            set +e
            if [ "$local_started" -eq 1 ]; then
              for name in libhsa-runtime64.0.1.0.dylib libhsa-runtime64.1.dylib libhsa-runtime64.dylib; do
                if [ ! -e "$local_stage/$name" ] && [ ! -L "$local_stage/$name" ]; then
                  /bin/rm -f "$local_dst/$name" || result=1
                fi
                if [ -e "$local_backup/$name" ] || [ -L "$local_backup/$name" ]; then
                  /bin/mv "$local_backup/$name" "$local_dst/$name" || result=1
                fi
              done
            fi
            if [ "$fw_new" -eq 1 ]; then /bin/rm -rf "$fw_dst" || result=1; fi
            if [ "$fw_old" -eq 1 ]; then /bin/mv "$fw_backup" "$fw_dst" || result=1; fi
            if [ "$app_new" -eq 1 ]; then /bin/rm -rf "$dst" || result=1; fi
            if [ "$app_old" -eq 1 ]; then /bin/mv "$backup" "$dst" || result=1; fi
            if [ "$runtime_new" -eq 1 ]; then /bin/rm -rf "$runtime_dst" || result=1; fi
            if [ "$runtime_old" -eq 1 ]; then
              /bin/mv "$runtime_backup" "$runtime_dst" || result=1
            fi
          fi
          /bin/rm -rf "$stage" "$runtime_stage" "$local_stage" "$fw_stage"
          exit "$result"
        }
        [ ! -L "$src" ] && [ -d "$src" ]
        [ ! -e "$stage" ] && [ ! -L "$stage" ]
        [ ! -e "$backup" ] && [ ! -L "$backup" ]
        [ ! -e "$runtime_stage" ] && [ ! -L "$runtime_stage" ]
        [ ! -e "$runtime_backup" ] && [ ! -L "$runtime_backup" ]
        [ ! -e "$local_stage" ] && [ ! -L "$local_stage" ]
        [ ! -e "$local_backup" ] && [ ! -L "$local_backup" ]
        [ ! -L "$dst" ] && [ ! -L "$runtime_dst" ]
        [ ! -L /usr/local ] && [ ! -L "$local_dst" ]
        [ ! -L /Library/MacAMDGPU ]
        trap finish EXIT
        /bin/mkdir -p "$local_dst"
        [ -d "$local_dst" ] && [ ! -L "$local_dst" ]
        /usr/bin/ditto "$src" "$stage"
        /usr/bin/codesign --verify --deep --strict "$stage"
        [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$stage/Contents/Info.plist")" = \(shellQuote(hostBundleIdentifier)) ]
        [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$stage/Contents/Library/SystemExtensions/\(dextBundleIdentifier).dext/Info.plist")" = \(shellQuote(dextBundleIdentifier)) ]
        /usr/bin/codesign -dv "$stage" 2>&1 | /usr/bin/grep -Fxq \(shellQuote("TeamIdentifier=" + expectedTeamIdentifier))
        /usr/bin/codesign -dv "$stage/Contents/Library/SystemExtensions/\(dextBundleIdentifier).dext" 2>&1 | /usr/bin/grep -Fxq \(shellQuote("TeamIdentifier=" + expectedTeamIdentifier))
        [ -d "$stage/Contents/Resources/Runtime" ] && [ ! -L "$stage/Contents/Resources/Runtime" ]
        /bin/mkdir -p /Library/MacAMDGPU
        [ -d /Library/MacAMDGPU ] && [ ! -L /Library/MacAMDGPU ]
        /usr/bin/ditto "$stage/Contents/Resources/Runtime" "$runtime_stage"
        for entry in "$runtime_stage"/* "$runtime_stage"/.[!.]* "$runtime_stage"/..?*; do
          if [ ! -e "$entry" ] && [ ! -L "$entry" ]; then continue; fi
          case "${entry##*/}" in
            libhsa-runtime64.0.1.0.dylib|libhsa-runtime64.1.dylib|libhsa-runtime64.dylib) ;;
            *) exit 1 ;;
          esac
        done
        for lib in libhsa-runtime64.0.1.0.dylib; do
          [ -f "$runtime_stage/$lib" ] && [ ! -L "$runtime_stage/$lib" ]
          /usr/bin/codesign --verify --strict "$runtime_stage/$lib"
          /usr/bin/codesign -dv "$runtime_stage/$lib" 2>&1 | /usr/bin/grep -Fxq \(shellQuote("TeamIdentifier=" + expectedTeamIdentifier))
        done
        [ -L "$runtime_stage/libhsa-runtime64.1.dylib" ] && [ "$(/usr/bin/readlink "$runtime_stage/libhsa-runtime64.1.dylib")" = libhsa-runtime64.0.1.0.dylib ]
        [ -L "$runtime_stage/libhsa-runtime64.dylib" ] && [ "$(/usr/bin/readlink "$runtime_stage/libhsa-runtime64.dylib")" = libhsa-runtime64.1.dylib ]
        /bin/mkdir "$local_stage" "$local_backup"
        /usr/bin/ditto "$runtime_stage/libhsa-runtime64.0.1.0.dylib" "$local_stage/libhsa-runtime64.0.1.0.dylib"
        /bin/ln -s libhsa-runtime64.0.1.0.dylib "$local_stage/libhsa-runtime64.1.dylib"
        /bin/ln -s libhsa-runtime64.1.dylib "$local_stage/libhsa-runtime64.dylib"
        if [ -e "$runtime_dst" ]; then /bin/mv "$runtime_dst" "$runtime_backup"; runtime_old=1; fi
        /bin/mv "$runtime_stage" "$runtime_dst"
        runtime_new=1
        if [ -e "$dst" ]; then /bin/mv "$dst" "$backup"; app_old=1; fi
        /bin/mv "$stage" "$dst"
        app_new=1
        local_started=1
        for name in libhsa-runtime64.0.1.0.dylib libhsa-runtime64.1.dylib libhsa-runtime64.dylib; do
          if [ -e "$local_dst/$name" ] || [ -L "$local_dst/$name" ]; then
            /bin/mv "$local_dst/$name" "$local_backup/$name"
          fi
          /bin/mv "$local_stage/$name" "$local_dst/$name"
        done
        # Firmware: the whole bundled amdgpu directory, served on demand to the
        # driver by whichever process initializes the GPU.
        if [ -d "$dst/Contents/Resources/firmware" ]; then
          /bin/mkdir -p "$fw_root"
          [ -d "$fw_root" ]
          [ ! -L "$fw_root" ]
          [ ! -e "$fw_stage" ]
          /usr/bin/ditto "$dst/Contents/Resources/firmware" "$fw_stage"
          if [ -e "$fw_dst" ] || [ -L "$fw_dst" ]; then /bin/mv "$fw_dst" "$fw_backup"; fw_old=1; fi
          /bin/mv "$fw_stage" "$fw_dst"
          fw_new=1
        fi
        committed=1
        if [ "$fw_old" -eq 1 ]; then /bin/rm -rf "$fw_backup"; fi
        printf 'Installed %s\\nPrevious app backup: %s\\nPrevious runtime backup: %s\\nPrevious /usr/local HSA backup: %s\\n' "$dst" "$backup" "$runtime_backup" "$local_backup"
        """
        // AppleScript handles the administrator prompt. The command is fixed;
        // only paths, each shell quoted, vary. A failure retains the old app.
        let appleScript = "do shell script \"" + command
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n") + "\" with administrator privileges"
        let result = run("/usr/bin/osascript", ["-e", appleScript])
        return (result.status == 0, result.output.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    static func isWaitingForReboot(_ version: (short: String, build: String)) -> Bool {
        let result = run("/usr/bin/systemextensionsctl", ["list"])
        guard result.status == 0 else { return false }
        return result.output.split(separator: "\n").contains { line in
            line.contains(dextBundleIdentifier) &&
            line.contains("(\(version.short)/\(version.build))") &&
            line.contains("waiting to upgrade on reboot")
        }
    }
}

final class ExtensionActivator: NSObject, OSSystemExtensionRequestDelegate {
    private var request: OSSystemExtensionRequest?

    func activate() {
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        self.request = request
        request.delegate = self
        print("Checking installed extension \(dextBundleIdentifier)")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private var bundledVersion: (short: String, build: String)? {
        let plistURL = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext/Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(
                from: data, options: [], format: nil) as? [String: Any],
              let short = plist["CFBundleShortVersionString"] as? String,
              let build = plist["CFBundleVersion"] as? String else { return nil }
        return (short, build)
    }

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        guard request === self.request else { return }
        guard let version = bundledVersion else {
            fputs("Bundled extension version is missing.\n", stderr)
            exit(1)
        }
        if properties.contains(where: {
            $0.isEnabled && !$0.isUninstalling &&
            $0.bundleShortVersion == version.short &&
            $0.bundleVersion == version.build
        }) {
            print("Extension \(version.short) (\(version.build)) is already enabled.")
            exit(0)
        }
        let activation = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        self.request = activation
        activation.delegate = self
        print("Requesting activation of \(dextBundleIdentifier) \(version.short) (\(version.build))")
        OSSystemExtensionManager.shared.submitRequest(activation)
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        if existing.bundleShortVersion == ext.bundleShortVersion &&
           existing.bundleVersion == ext.bundleVersion {
            print("Identical extension build is already installed; replacement skipped.")
            return .cancel
        }
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("Approval required: open System Settings → General → Login Items & Extensions → Driver Extensions.")
        print("The activation request will continue after approval.")
        if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        self.request = nil
        let exitStatus: Int32
        switch result {
        case .completed:
            print("System extension registered. Check attachment with systemextensionsctl list.")
            exitStatus = 0
        case .willCompleteAfterReboot:
            print("System extension activation is pending; attachment has not been verified.")
            exitStatus = 2
        @unknown default:
            print("System extension request finished with result: \(result)")
            exitStatus = 1
        }
        exit(exitStatus)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        self.request = nil
        fputs("System extension activation failed: \(error)\n", stderr)
        exit(1)
    }
}

// ----------------------------------------------------------------
// MARK: - User-client selectors (MUST match dext/sources/
// MacLinuxGPUXcode.mm — the kMacAMDGPUMethod* enum, which is kept
// identical to mac_amdgpu/MacAMDGPU.cpp).
// ----------------------------------------------------------------
private let kSelPing:             UInt32 = 0
private let kSelGetIdentity:      UInt32 = 1
private let kSelGetBARInfo:       UInt32 = 2
private let kSelSetupInterrupts:  UInt32 = 3
private let kSelWaitInterrupt:    UInt32 = 4   // async
private let kSelSetIRQMask:       UInt32 = 5
private let kSelAllocateDMABuffer: UInt32 = 6
private let kSelFreeDMABuffer:    UInt32 = 7
private let kSelResetDevice:      UInt32 = 8
private let kSelInitDevice:       UInt32 = 9
private let kSelLoadFirmware:     UInt32 = 10
private let kSelSetIPBase:        UInt32 = 11
private let kSelGetIPBase:        UInt32 = 12
private let kSelLoadDiscoveryBin: UInt32 = 13
private let kSelSubmitTestPM4:    UInt32 = 14
private let kSelSDMACopyTest:     UInt32 = 15
private let kSelBOAlloc:          UInt32 = 16
private let kSelBOFree:           UInt32 = 17
private let kSelBOGetInfo:        UInt32 = 18
private let kSelSubmitIB:         UInt32 = 19
private let kSelWaitFence:        UInt32 = 20
private let kSelQueryInfo:        UInt32 = 21
private let kSelMESAddQueue:      UInt32 = 22
private let kSelGetDiagnostics:   UInt32 = 23
private let kSelDumpTMR:          UInt32 = 24
private let kSelDumpPSP:          UInt32 = 25
private let kSelDumpCmdBuf:       UInt32 = 26
private let kSelLiveStatus:       UInt32 = 30
private let kSelDisableSmuFeatures: UInt32 = 33
private let kSelBOMap:            UInt32 = 36
private let kSelReleaseQuarantine: UInt32 = 61 // entitled; see session_state.h
private let kSelPower:            UInt32 = 83 // device power; see power_state.h

// Observer clients (dext/sources/session_state.h) read cached state only and
// may attach while a session closes or stays quarantined.
private let kUserClientSession:  UInt32 = 0
private let kUserClientObserver: UInt32 = 1
private let kQuerySessionState:  UInt64 = 0x4c534553
private let kSessionStateWords = 9
private let kIOReturnUnsupportedValue = kern_return_t(bitPattern: 0xe00002c7)
private let kQueryPowerState:    UInt64 = 0x4c505752
private let kPowerStateWords = 12
private let kPowerOpQuery:   UInt64 = 0
private let kPowerOpPrepare: UInt64 = 1
private let kPowerOpResume:  UInt64 = 2

/// The cached device power snapshot (QueryInfo "LPWR"); names match
/// dext/sources/power_state.h and scripts/read-driver-log.py.
struct PowerState {
    let state: UInt64
    let generation: UInt64
    let flags: UInt64
    let cause: UInt64
    let error: Int64
    let holds: UInt64

    static let states = ["active", "suspending", "suspended", "resuming", "lost"]
    static let causes = ["none", "client prepare", "client resume", "holding client closed", "system sleep",
                         "system wake", "device low power", "device on", "KFD suspend failed",
                         "KFD resume failed", "device gone after wake", "re-probed", "session closed"]

    init?(_ values: [UInt64]) {
        guard values.count == kPowerStateWords, values[0] == 1 else { return nil }
        state = values[1]; generation = values[2]; flags = values[3]
        cause = values[4]; error = Int64(bitPattern: values[5]); holds = values[6]
    }

    var vramPreserved: Bool { flags & 1 != 0 }
    var summary: String {
        let name = SessionState.name(PowerState.states, state)
        let why = SessionState.name(PowerState.causes, cause)
        let memory = vramPreserved ? "VRAM preserved" : "VRAM lost"
        return "Power: \(name) (\(why)\(error != 0 ? ", error \(error)" : ""); \(memory); " +
            "\(holds) low-power hold(s); generation \(generation))"
    }
}

/// The cached session snapshot (QueryInfo "LSES"); names match
/// dext/sources/session_state.h and scripts/read-driver-log.py.
struct SessionState {
    let flags: UInt64
    let cause: UInt64
    let causeCode: Int64
    let releaseBlocker: UInt64
    let generation: UInt64
    let participants: UInt64

    var closing: Bool { flags & (1 << 0) != 0 }
    var quarantined: Bool { flags & (1 << 1) != 0 }
    var releasable: Bool { flags & (1 << 6) != 0 }
    var restartRequired: Bool { flags & (1 << 7) != 0 }

    static let causes = ["none", "raw BAR mapping lifetime uncertain", "DMA shutdown reservation failed",
                         "GPU completion uncertain (compute stop)", "interrupt cancellation failed",
                         "endpoint isolation failed", "DMA backing retained at fini", "definite PCI fault",
                         "probe DMA reservation failed", "failed-probe ownership retained",
                         "probe DMA commit failed", "shared-session client cleanup failed",
                         "release attempt failed"]
    static let blockers = ["ready", "not quarantined", "interrupt drain pending",
                           "interrupt cancellation failed", "upstream driver or runtime device retained",
                           "compute work retained", "raw BAR mapping held", "session clients still attached",
                           "DMA backing still owned", "definite PCI fault", "PCI admission busy",
                           "endpoint reset failed during release"]
    static func name(_ table: [String], _ value: UInt64) -> String {
        value < UInt64(table.count) ? table[Int(value)] : "unknown (\(value))"
    }

    var summary: String {
        if restartRequired {
            return "Restart required, do not kill the driver (quarantined: \(SessionState.name(SessionState.causes, cause)), code \(causeCode))"
        }
        if releasable {
            return "Quarantined but quiescent (\(SessionState.name(SessionState.causes, cause))); release it or deactivate the extension, do not kill the driver"
        }
        if quarantined {
            return "Quarantined, release pending: \(SessionState.name(SessionState.blockers, releaseBlocker)); do not kill the driver"
        }
        return closing ? "Session closing" : "No quarantine (session generation \(generation), \(participants) client(s))"
    }
}

// ----------------------------------------------------------------
// MARK: - The MacLinuxGPUHost (the host app's driver).
// ----------------------------------------------------------------
final class MacLinuxGPUHost {
    private(set) var ucConn: io_connect_t = 0
    private(set) var isOpen: Bool = false
    private var logLines: [String] = []

    // The dext's PCI identity (from GetIdentity).
    private(set) var pciBus: UInt8 = 0
    private(set) var pciDev: UInt8 = 0
    private(set) var pciFn:  UInt8 = 0
    private(set) var vid:    UInt16 = 0
    private(set) var did:    UInt16 = 0
    private(set) var classCode: UInt32 = 0
    private(set) var revision:  UInt8 = 0
    private(set) var subsystemVendor: UInt16 = 0
    private(set) var subsystemDevice: UInt16 = 0

    // MARK: Lifecycle

    /// Open the UserClient (IOServiceGetMatchingService + IOServiceOpen).
    /// The single-tenant client (type 0).  Returns true on success.
    @discardableResult
    func openUserClient(allowUnverified: Bool = false, observer: Bool = false) -> Bool {
        if isOpen { return true }

        // Match the dext's IOUserService.  The dext's Info.plist
        // IOKitPersonalities (the IOPCIDevice provider) makes IOKit
        // instantiate MacLinuxGPU on PCI match; the UserClient is spawned
        // per IOServiceOpen (the NewUserClient, type 0).
        //
        // The match is on the "IOUserService" class (the generic UserClient
        // class) filtered by the dext's product.  The mac_amdgpu reference
        // uses IOServiceGetMatchingServices on "IOUserService".
        guard let matching = IOServiceMatching("IOUserService") else {
            append("openUserClient: IOServiceMatching(IOUserService) failed")
            return false
        }

        var iter: io_iterator_t = 0
        let matched = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                   matching as CFDictionary,
                                                   &iter)
        guard matched == kIOReturnSuccess else {
            append(String(format: "openUserClient: service matching failed (kr=%#x)", matched))
            return false
        }
        defer { IOObjectRelease(iter) }

        // Find the dext's service by the dext's bundle identifier, never by
        // a PCI device ID: the dext attaches to whichever AMD GPU matched.
        var svc = IOIteratorNext(iter)
        while svc != 0 {
            var props: Unmanaged<CFMutableDictionary>?
            let kr = IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0)
            let dict = props?.takeRetainedValue() as? [String: Any]
            if kr == kIOReturnSuccess,
               dict?["CFBundleIdentifier"] as? String == dextBundleIdentifier {
                var connection: io_connect_t = 0
                var opened = IOServiceOpen(svc, mach_task_self_,
                                           observer ? kUserClientObserver : kUserClientSession,
                                           &connection)
                if observer && opened == kIOReturnUnsupportedValue {
                    // Drivers without observer clients accept only a session client.
                    opened = IOServiceOpen(svc, mach_task_self_, kUserClientSession, &connection)
                }
                IOObjectRelease(svc)
                if opened != kIOReturnSuccess {
                    append(String(format: "openUserClient: IOServiceOpen failed (kr=%#x)", opened))
                    if connection != 0 { IOServiceClose(connection) }
                    return false
                }
                ucConn = connection
                isOpen = true
                append("openUserClient: UserClient opened (conn=%d)", ucConn)
                return true
            }
            IOObjectRelease(svc)
            svc = IOIteratorNext(iter)
        }

        append("openUserClient: no MacLinuxGPU service found for \(dextBundleIdentifier)")
        return false
    }

    /// Close the UserClient (IOServiceClose).  The lifecycle teardown.
    @discardableResult
    func closeUserClient() -> Bool {
        guard isOpen else { return true }
        let kr = IOServiceClose(ucConn)
        if kr != kIOReturnSuccess {
            append(String(format: "closeUserClient: IOServiceClose failed (kr=%#x)", kr))
            return false
        }
        ucConn = 0
        isOpen = false
        append("closeUserClient: UserClient closed")
        return true
    }

    // MARK: The selector-RPC (IOConnectCallScalarMethod / IOConnectCallStructMethod)

    /// Call a scalar selector (the mac_amdgpu reference's callScalar).
    /// The in scalars + the out scalars.  The data buffers are null (the
    /// scalar-only selectors: Ping, SetupInterrupts, etc.).
    func callScalar(_ selector: UInt32,
                    inScalars: [UInt64] = [],
                    outScalars: Int = 0) -> (kern_return_t, [UInt64]) {
        guard isOpen else { return (kIOReturnError, []) }
        let inCount = UInt32(inScalars.count)
        var outBuf = [UInt64](repeating: 0, count: max(1, outScalars))
        var outN = UInt32(outBuf.count)
        let kr: kern_return_t = inScalars.withUnsafeBufferPointer { ibuf in
            outBuf.withUnsafeMutableBufferPointer { obuf in
                IOConnectCallScalarMethod(ucConn, selector,
                                          ibuf.baseAddress,
                                          inCount,
                                          obuf.baseAddress,
                                          &outN)
            }
        }
        return (kr, Array(outBuf.prefix(Int(outN))))
    }

    /// Call a struct selector (the mac_amdgpu reference's callStruct).
    /// The in data buffer + the out data buffer.  Used by GetIdentity,
    /// GetBARInfo, AllocateDMABuffer, LoadFirmware.
    func callStruct(_ selector: UInt32,
                    inData: Data?, inSize: Int,
                    outSize: Int) -> (kern_return_t, Data) {
        guard isOpen else { return (kIOReturnError, Data()) }
        var outData = Data(count: max(1, outSize))
        var outCnt = size_t(max(1, outSize))
        let kr: kern_return_t = outData.withUnsafeMutableBytes { outPtr -> kern_return_t in
            if let inData = inData {
                return inData.withUnsafeBytes { inPtr in
                    IOConnectCallStructMethod(ucConn, selector,
                                              inPtr.baseAddress, inSize,
                                              outPtr.baseAddress, &outCnt)
                }
            } else {
                return IOConnectCallStructMethod(ucConn, selector,
                                                 nil, 0,
                                                 outPtr.baseAddress, &outCnt)
            }
        }
        return (kr, outData.prefix(Int(min(outCnt, size_t(max(0, outSize))))))
    }

    // MARK: The key selectors

    /// Ping — the round-trip plumbing test.
    func ping() -> Bool {
        let (kr, out) = callScalar(kSelPing, outScalars: 1)
        if kr != kIOReturnSuccess || out.first != 0xA117AB1E {
            append(String(format: "ping: IOConnectCallScalarMethod failed (kr=%#x)", kr))
            return false
        }
        append("ping: OK (magic=%#llx)", out.first ?? 0)
        return true
    }

    /// GetIdentity — the PCI bus/dev/fn + VID/DID.
    func getIdentity() -> Bool {
        // Seven scalars from older drivers; nine add the subsystem IDs.
        let (kr, values) = callScalar(kSelGetIdentity, outScalars: 9)
        if kr != kIOReturnSuccess || values.count < 7 {
            append(String(format: "getIdentity: failed (kr=%#x)", kr))
            return false
        }
        pciBus = UInt8(truncatingIfNeeded: values[0])
        pciDev = UInt8(truncatingIfNeeded: values[1])
        pciFn = UInt8(truncatingIfNeeded: values[2])
        vid = UInt16(truncatingIfNeeded: values[3])
        did = UInt16(truncatingIfNeeded: values[4])
        classCode = UInt32(truncatingIfNeeded: values[5])
        revision = UInt8(truncatingIfNeeded: values[6])
        if values.count >= 9 {
            subsystemVendor = UInt16(truncatingIfNeeded: values[7])
            subsystemDevice = UInt16(truncatingIfNeeded: values[8])
        }
        append(String(format: "getIdentity: %02x:%02x.%u vid=%04x did=%04x class=%06x rev=%02x subsystem=%04x:%04x",
                      pciBus, pciDev, pciFn, vid, did, classCode, revision,
                      subsystemVendor, subsystemDevice))
        // Any AMD function the dext matched; upstream decides support.
        return vid == 0x1002
    }

    /// A display name built only from what the dext reports: the PCI
    /// identity and, once upstream has initialized the device, the GC IP
    /// version read from the on-die IP discovery table (QueryInfo tag 1).
    func deviceDescription() -> String {
        var text = String(format: "AMD GPU %04x:%04x rev %02x (subsystem %04x:%04x)",
                          vid, did, revision, subsystemVendor, subsystemDevice)
        let (kr, gfx) = callScalar(kSelQueryInfo, inScalars: [1], outScalars: 3)
        if kr == kIOReturnSuccess, gfx.count == 3, gfx[0] != 0 {
            text += String(format: " (GC %llu.%llu.%llu)", gfx[0], gfx[1], gfx[2])
        }
        return text
    }

    /// GetBARInfo — the per-BAR memoryIndex/size/type.
    func getBARInfo(bar: UInt8) -> (memoryIndex: UInt8, size: UInt64, type: UInt8)? {
        let (kr, values) = callScalar(kSelGetBARInfo,
                                      inScalars: [UInt64(bar)], outScalars: 3)
        guard kr == kIOReturnSuccess, values.count == 3 else { return nil }
        return (UInt8(truncatingIfNeeded: values[0]), values[1],
                UInt8(truncatingIfNeeded: values[2]))
    }

    /// SetupInterrupts — the MSI-X vectors + the IRQ shared page (T-irq-dext).
    func setupInterrupts() -> Bool {
        let (kr, out) = callScalar(kSelSetupInterrupts, inScalars: [0], outScalars: 1)
        if kr != kIOReturnSuccess {
            append(String(format: "setupInterrupts: failed (kr=%#x)", kr))
            return false
        }
        append("setupInterrupts: OK (vectors=%d)", out.first ?? 0)
        return true
    }

    /// ResetDevice — the FLR (Function Level Reset).
    func resetDevice() -> Bool {
        let (kr, _) = callScalar(kSelResetDevice, inScalars: [], outScalars: 0)
        if kr != kIOReturnSuccess {
            append(String(format: "resetDevice: failed (kr=%#x)", kr))
            return false
        }
        append("resetDevice: FLR issued")
        return true
    }

    /// InitDevice — invoke the upstream AMDGPU PCI probe.  Upstream requests
    /// firmware from inside the probe, so the firmware servicer answers the
    /// dext's on-demand requests from the installed firmware root until the
    /// call returns.  Without a servicer the dext falls back to its optional
    /// embedded table and otherwise fails requests with -ENOENT.
    func initDevice() -> Bool {
        var service: OpaquePointer?
        let started = mlg_fw_service_start_connection(UInt32(ucConn), nil, &service)
        if started != 0 {
            append("initDevice: firmware servicer unavailable (%d); embedded fallback only", started)
        }
        let (kr, _) = callScalar(kSelInitDevice, inScalars: [], outScalars: 0)
        if let service {
            append("initDevice: firmware servicer served %llu file(s), %llu not found",
                   mlg_fw_service_served(service), mlg_fw_service_missing(service))
            mlg_fw_service_stop(service)
        }
        _ = liveStatus()
        if kr != kIOReturnSuccess {
            append(String(format: "initDevice: failed (kr=%#x)", kr))
            return false
        }
        append("initDevice: upstream PCI probe completed")
        return true
    }

    /// AllocateDMABuffer — the coherent DMA buffer (T-dma-dart-dext).
    /// Returns (cpu_addr, iova).
    func allocateDMABuffer(size: Int) -> (cpu: UInt64, iova: UInt64)? {
        var sizeData = Data()
        withUnsafeBytes(of: UInt64(size)) { sizeData.append(contentsOf: $0) }
        let (kr, outData) = callStruct(kSelAllocateDMABuffer,
                                       inData: sizeData, inSize: 8, outSize: 16)
        if kr != kIOReturnSuccess { return nil }
        guard outData.count >= 16 else { return nil }
        let bytes = [UInt8](outData)
        let cpu = UInt64(bytes[0]) | (UInt64(bytes[1]) << 8) | (UInt64(bytes[2]) << 16) | (UInt64(bytes[3]) << 24) | (UInt64(bytes[4]) << 32) | (UInt64(bytes[5]) << 40) | (UInt64(bytes[6]) << 48) | (UInt64(bytes[7]) << 56)
        let iova = UInt64(bytes[8]) | (UInt64(bytes[9]) << 8) | (UInt64(bytes[10]) << 16) | (UInt64(bytes[11]) << 24) | (UInt64(bytes[12]) << 32) | (UInt64(bytes[13]) << 40) | (UInt64(bytes[14]) << 48) | (UInt64(bytes[15]) << 56)
        append("allocateDMABuffer: cpu=0x%llx iova=0x%llx (size=%d)",
               cpu, iova, size)
        return (cpu, iova)
    }

    /// FreeDMABuffer — free the coherent buffer.
    func freeDMABuffer(cpu: UInt64) -> Bool {
        var cpuData = Data()
        withUnsafeBytes(of: cpu) { cpuData.append(contentsOf: $0) }
        let (kr, _) = callStruct(kSelFreeDMABuffer,
                                 inData: cpuData, inSize: 8, outSize: 0)
        if kr != kIOReturnSuccess {
            append(String(format: "freeDMABuffer: failed (kr=%#x)", kr))
            return false
        }
        return true
    }

    // MARK: The firmware push (optional preload).
    //
    // The normal path is on demand (see initDevice).  `fw <name>` pushes one
    // file over the LoadFirmware selector before InitDevice, for example to
    // test a replacement image.  <name> is the exact request_firmware() name
    // ("amdgpu/<file>.bin"), resolved against the firmware root.

    /// $MAC_LINUXGPU_FIRMWARE_ROOT or the installed firmware root.
    private var firmwareRoot: URL {
        let configured = ProcessInfo.processInfo.environment["MAC_LINUXGPU_FIRMWARE_ROOT"]
        return URL(fileURLWithPath: configured ?? firmwareRootPath, isDirectory: true)
    }

    /// Push one firmware file by its request name.
    func loadFirmware(_ name: String) -> Bool {
        guard !name.hasPrefix("/"), !name.split(separator: "/").contains("..") else {
            append("loadFirmware: %@ is not a relative firmware name", name)
            return false
        }
        let url = firmwareRoot.appendingPathComponent(name)
        guard let data = try? Data(contentsOf: url) else {
            append("loadFirmware: failed to read %@", url.path)
            return false
        }

        // Build the packet: { uint32_t name_len; char name[128]; blob }.
        let nameBytes = Array(name.utf8)
        guard nameBytes.count <= 128 else {
            append("loadFirmware: name too long (>128 bytes)")
            return false
        }
        var packet = Data()
        var nameLen: UInt32 = UInt32(nameBytes.count)
        withUnsafeBytes(of: &nameLen) { packet.append(contentsOf: $0) }
        packet.append(contentsOf: nameBytes)
        packet.append(Data(repeating: 0, count: 128 - nameBytes.count))
        packet.append(data)

        let (kr, _) = callStruct(kSelLoadFirmware,
                                 inData: packet, inSize: packet.count,
                                 outSize: 0)
        if kr != kIOReturnSuccess {
            append(String(format: "loadFirmware(%@): failed (kr=%#x)", name, kr))
            return false
        }
        append("loadFirmware(%@): OK (%d bytes)", name, data.count)
        return true
    }

    // MARK: Status read (the bringup state, the error log)

    /// Probe diagnostics use a namespaced QueryInfo tag; selector 30 remains
    /// reserved for the reference driver's live engine register snapshot.
    func liveStatus() -> Bool {
        let (kr, values) = callScalar(kSelQueryInfo, inScalars: [0x4c50524f], outScalars: 5)
        guard kr == kIOReturnSuccess, values.count == 5 else {
            append(String(format: "probe status: failed (kr=%#x)", kr))
            return false
        }
        append("probe: attempted=%llu bound=%llu result=%lld transport_fault=%llu offset=0x%llx",
               values[0], values[1], Int64(bitPattern: values[2]), values[3], values[4])
        return true
    }

    /// Cached session lifecycle and quarantine cause; never claims PCI.
    func sessionState() -> SessionState? {
        let (kr, values) = callScalar(kSelQueryInfo, inScalars: [kQuerySessionState],
                                      outScalars: kSessionStateWords)
        guard kr == kIOReturnSuccess, values.count == kSessionStateWords, values[0] == 1 else {
            append(String(format: "session state: unavailable (kr=%#x)", kr))
            return nil
        }
        let state = SessionState(flags: values[1], cause: values[2],
                                 causeCode: Int64(bitPattern: values[3]),
                                 releaseBlocker: values[6], generation: values[7],
                                 participants: values[8])
        append("session: \(state.summary)")
        return state
    }

    /// Device power (cached); a PREPARE or RESUME request needs the
    /// session-release entitlement on an observer client and lasts as long
    /// as this client stays open.
    func power(_ op: UInt64 = kPowerOpQuery) -> PowerState? {
        let (kr, values) = op == kPowerOpQuery
            ? callScalar(kSelQueryInfo, inScalars: [kQueryPowerState], outScalars: kPowerStateWords)
            : callScalar(kSelPower, inScalars: [op], outScalars: kPowerStateWords)
        guard kr == kIOReturnSuccess, let state = PowerState(values) else {
            append(String(format: "power: unavailable (kr=%#x)", kr))
            return nil
        }
        append(state.summary)
        return state
    }

    /// Release a quarantined session once the driver's cached state proves it
    /// quiescent. Requires the session-release entitlement.
    func releaseQuarantine() -> Bool {
        let (kr, values) = callScalar(kSelReleaseQuarantine, outScalars: 2)
        guard kr == kIOReturnSuccess, values.count == 2 else {
            append(String(format: "release: refused (kr=%#x)", kr))
            return false
        }
        let blocker = SessionState.name(SessionState.blockers, values[1])
        if values[0] == UInt64(UInt32(bitPattern: kIOReturnSuccess)) {
            append("release: quarantined session released; the driver is reusable")
            return true
        }
        append("release: not released (\(blocker))")
        return false
    }

    // MARK: Logging

    private func append(_ fmt: String, _ args: CVarArg...) {
        let line = String(format: fmt, arguments: args)
        logLines.append(line)
        print("[mac.linuxgpu.host] " + line)
    }

    var log: [String] { return logLines }
}

// The normal app launch follows the mac_amdgpu self-installing host flow. The
// CLI commands remain available to activate.sh and diagnostics tools.
@MainActor
private final class InstallerController: NSObject, ObservableObject,
                                         @preconcurrency OSSystemExtensionRequestDelegate {
    @Published var status = "Starting…"
    @Published var log = ""
    @Published var isWorking = false
    @Published var needsCopy = false
    @Published var bundledVersion = "—"
    @Published var registeredVersion = "—"
    @Published var runningStatus = "not checked"

    private var request: OSSystemExtensionRequest?
    private var checkingProperties = false
    private var hasStarted = false

    func start() {
        guard !hasStarted else { return }
        hasStarted = true
        let source = Bundle.main.bundleURL.standardizedFileURL
        needsCopy = source != installedAppURL.standardizedFileURL
        if let version = InstallPackage.version(at: source) {
            bundledVersion = "\(version.short) (\(version.build))"
        }
        append("Opened \(source.path)")
        if needsCopy {
            status = "Ready to install"
            append("Click Install to install the app and bundled GPU runtime, then activate the driver.")
        } else {
            status = "Checking driver…"
            activate()
        }
    }

    func install() {
        guard !isWorking, needsCopy else { return }
        isWorking = true
        status = "Verifying package…"
        let source = Bundle.main.bundleURL
        append("Verifying the signed host app and embedded driver.")
        Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) { () -> (Bool, String) in
                if let error = InstallPackage.validate(source) { return (false, error) }
                return InstallPackage.install(source)
            }.value
            guard let self else { return }
            if !result.0 {
                self.status = "Install failed"
                self.append(result.1)
                self.isWorking = false
                return
            }
            self.append(result.1)
            self.status = "Opening installed app…"
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = true
            configuration.createsNewApplicationInstance = true
            NSWorkspace.shared.openApplication(at: installedAppURL, configuration: configuration) {
                app, error in
                Task { @MainActor in
                    guard let app, error == nil,
                          app.processIdentifier != ProcessInfo.processInfo.processIdentifier else {
                        self.status = "Could not open installed app"
                        self.append(error?.localizedDescription ?? "The installed app did not launch separately.")
                        self.isWorking = false
                        return
                    }
                    self.append("Installed app launched (PID \(app.processIdentifier)). It will request driver activation.")
                    NSApp.terminate(nil)
                }
            }
        }
    }

    func activate() {
        guard !needsCopy, !isWorking, request == nil else { return }
        if let error = InstallPackage.validate(Bundle.main.bundleURL) {
            status = "Package verification failed"
            append(error)
            return
        }
        isWorking = true
        checkingProperties = true
        status = "Checking installed extension…"
        let check = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        request = check
        check.delegate = self
        OSSystemExtensionManager.shared.submitRequest(check)
    }

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        guard self.request === request, checkingProperties else { return }
        let active = properties.first { $0.isEnabled && !$0.isUninstalling }
            ?? properties.first
        registeredVersion = active.map { "\($0.bundleShortVersion) (\($0.bundleVersion))" }
            ?? "not registered"
        if let active, active.isEnabled && !active.isUninstalling,
           registeredVersion == bundledVersion {
            append("Bundled extension is registered and enabled; checking the actual UserClient.")
            self.request = nil
            checkingProperties = false
            verifyAttachedDriver()
            return
        }
        if let bundled = InstallPackage.version(at: Bundle.main.bundleURL),
           InstallPackage.isWaitingForReboot(bundled) {
            self.request = nil
            checkingProperties = false
            isWorking = false
            status = "Activation pending"
            runningStatus = "replacement staged"
            append("macOS has staged this extension build; attachment has not been verified.")
            return
        }
        checkingProperties = false
        status = "Requesting activation…"
        append("Requesting activation of \(dextBundleIdentifier) \(bundledVersion).")
        let activation = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        self.request = activation
        activation.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activation)
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        guard self.request === request else { return .cancel }
        if existing.bundleShortVersion == ext.bundleShortVersion &&
           existing.bundleVersion == ext.bundleVersion {
            append("The same extension build is already registered; replacement skipped.")
            return .cancel
        }
        append("Replacing \(existing.bundleShortVersion) (\(existing.bundleVersion)) with "
               + "\(ext.bundleShortVersion) (\(ext.bundleVersion)).")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        guard self.request === request else { return }
        status = "Approval required in System Settings"
        append("Open System Settings → General → Login Items & Extensions → Driver Extensions and enable MacLinuxGPU.")
        if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        guard self.request === request else { return }
        self.request = nil
        checkingProperties = false
        switch result {
        case .completed:
            append("macOS completed extension registration; checking attachment.")
            registeredVersion = bundledVersion
            verifyAttachedDriver()
        case .willCompleteAfterReboot:
            status = "Activation pending"
            runningStatus = "replacement staged"
            append("macOS deferred activation of this extension; attachment has not been verified.")
            isWorking = false
        @unknown default:
            status = "Unknown activation result"
            append("macOS returned an unknown extension result: \(result.rawValue)")
            isWorking = false
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        guard self.request === request else { return }
        self.request = nil
        checkingProperties = false
        isWorking = false
        status = "Activation failed"
        append(error.localizedDescription)
    }

    func verifyAttachedDriver() {
        guard !needsCopy, request == nil else { return }
        isWorking = true
        status = "Checking GPU attachment…"
        runningStatus = "checking"
        Task { [weak self] in
            let (attached, result) = await Task.detached(priority: .utility) { () -> (Bool, String) in
                for attempt in 0..<45 {
                    let driver = MacLinuxGPUHost()
                    // A quarantined driver refuses session clients; an observer
                    // reports why and whether killing it would be unsafe.
                    let observer = MacLinuxGPUHost()
                    if observer.openUserClient(observer: true) {
                        let state = observer.sessionState()
                        _ = observer.closeUserClient()
                        if let state, state.quarantined {
                            return (false, state.summary)
                        }
                    }
                    if driver.openUserClient() {
                        let first = driver.ping() && driver.getIdentity()
                        if first {
                            let device = driver.deviceDescription()
                            try? await Task.sleep(nanoseconds: 2_000_000_000)
                            let second = driver.ping()
                            if !driver.closeUserClient() {
                                return (false, "UserClient close failed; attachment could not be verified")
                            }
                            if second { return (true, "\(device): UserClient identity verified and two pings passed") }
                        } else {
                            if !driver.closeUserClient() {
                                return (false, "UserClient close failed; attachment could not be verified")
                            }
                        }
                    }
                    if attempt < 44 { try? await Task.sleep(nanoseconds: 1_000_000_000) }
                }
                return (false, "No responding MacLinuxGPU UserClient is attached to an AMD GPU")
            }.value
            guard let self else { return }
            self.runningStatus = result
            if attached {
                self.status = "Driver attached"
                self.append("\(result). GPU initialization and compute are not verified by this check.")
            } else {
                self.status = "Driver not attached"
                self.append(result + ". Registration alone does not show that the dext is running.")
            }
            self.isWorking = false
        }
    }

    private func append(_ line: String) {
        log += "[\(Date().formatted(date: .omitted, time: .standard))] \(line)\n"
    }
}

private struct InstallerView: View {
    @ObservedObject var controller: InstallerController

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("MacLinuxGPU").font(.title.weight(.semibold))
                Spacer()
                Text(controller.status)
                    .foregroundStyle(controller.status == "Driver attached" ? .green : .orange)
            }
            Text("Upstream Linux amdgpu driver for AMD GPUs over Thunderbolt")
                .foregroundStyle(.secondary)
            HStack(spacing: 22) {
                label("Bundled", controller.bundledVersion)
                label("Registered", controller.registeredVersion)
                label("Running", controller.runningStatus)
            }
            Divider()
            HStack {
                if controller.needsCopy {
                    Button("Install to Applications") { controller.install() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(controller.isWorking)
                } else {
                    Button("Install Driver") { controller.activate() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(controller.isWorking)
                    Button("Verify Attachment") { controller.verifyAttachedDriver() }
                        .disabled(controller.isWorking)
                }
                Spacer()
                Button("Open Driver Extensions") {
                    if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
                        NSWorkspace.shared.open(url)
                    }
                }
            }
            ScrollView {
                Text(controller.log)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
            .background(Color(NSColor.textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .padding(18)
        .frame(minWidth: 640, minHeight: 380)
    }

    private func label(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.system(.callout, design: .monospaced))
                .lineLimit(2)
        }
    }
}

@MainActor
private final class HostApplicationDelegate: NSObject, NSApplicationDelegate {
    private let controller = InstallerController()
    private var window: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 700, height: 430),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "MacLinuxGPU Installer"
        window.center()
        window.contentView = NSHostingView(rootView: InstallerView(controller: controller))
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
        controller.start()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

// ----------------------------------------------------------------
// MARK: - GUI and command-line entry
// ----------------------------------------------------------------
// The entry point. This file is NOT named main.swift, so the entry point
// must be a @main attribute on a type (Swift requires this for non-main.swift
// files). The mac_amdgpu reference uses a main.swift file; we use @main
// instead (equivalent, no file rename needed).
// ----------------------------------------------------------------
@main
struct AppMain {
    static func main() {
        let args = CommandLine.arguments

        if args.count == 1 {
            let app = NSApplication.shared
            let delegate = HostApplicationDelegate()
            app.delegate = delegate
            app.setActivationPolicy(.regular)
            app.run()
            return
        }

        if args[1] == "activate" {
            let activator = ExtensionActivator()
            activator.activate()
            RunLoop.main.run()
            return
        }

        let host = MacLinuxGPUHost()
        // Cached-state commands use an observer client, which works while a
        // session closes or stays quarantined and never joins a session.
        let observerCommands: Set<String> = ["status", "session", "release", "power", "power-watch"]

        if !host.openUserClient(observer: observerCommands.contains(args[1])) {
            print("ERROR: failed to open the UserClient (is the dext activated, "
                  + "signed, and an AMD GPU attached over Thunderbolt?)")
            exit(1)
        }

        let commandStatus: Int32
        switch args[1] {
case "ping":
    commandStatus = host.ping() ? 0 : 1
case "identity":
    commandStatus = host.getIdentity() ? 0 : 1
    if commandStatus == 0 { print(host.deviceDescription()) }
case "bars":
    for bar in 0..<6 {
        if let info = host.getBARInfo(bar: UInt8(bar)) {
            print(String(format: "BAR%u: memoryIndex=%u size=%llu type=%u",
                         bar, info.memoryIndex, info.size, info.type))
        }
    }
    commandStatus = 0
case "setup-irq":
    commandStatus = host.setupInterrupts() ? 0 : 1
case "init":
    commandStatus = host.initDevice() ? 0 : 1
case "fw":
    if args.count >= 3 {
        commandStatus = host.loadFirmware(args[2]) ? 0 : 1
    } else {
        print("usage: MacLinuxGPUHostApp fw <name>")
        commandStatus = 1
    }
case "status":
    commandStatus = host.liveStatus() ? 0 : 1
    if let state = host.sessionState(), state.quarantined { print(state.summary) }
case "session":
    if let state = host.sessionState() {
        print(state.summary)
        commandStatus = state.restartRequired ? 2 : 0
    } else {
        commandStatus = 1
    }
case "release":
    commandStatus = host.releaseQuarantine() ? 0 : 1
    if let state = host.sessionState() { print(state.summary) }
case "power":
    if let state = host.power() {
        print(state.summary)
        commandStatus = 0
    } else {
        commandStatus = 1
    }
case "power-watch":
    // Stays running: asks the driver for low power when the Mac is about
    // to sleep and drops the request on wake, so clients quiesce cleanly
    // before the driver closes the session for the sleep. The request is
    // this client's: it ends with the process.
    let center = NSWorkspace.shared.notificationCenter
    let sleep = center.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { _ in
        if let state = host.power(kPowerOpPrepare) { print("will sleep: " + state.summary) }
    }
    let wake = center.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { _ in
        if let state = host.power(kPowerOpResume) { print("did wake: " + state.summary) }
    }
    if let state = host.power() { print(state.summary) }
    print("watching sleep and wake; Ctrl-C to stop")
    RunLoop.main.run()
    center.removeObserver(sleep)
    center.removeObserver(wake)
    commandStatus = 0
case "close":
    commandStatus = 0
default:
    print("unknown command: \(args[1])")
    commandStatus = 1
}
        let closed = host.closeUserClient()
        exit(closed ? commandStatus : 1)
    }
}
