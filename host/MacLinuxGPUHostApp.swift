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
import Security
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
            // An earlier upgrade may have left the previous driver attached.
            exit(DriverUpgrade.retirePrevious(force: false, timeout: 90, report: { print($0) }) ? 0 : 3)
        }
        // macOS replaces a running driver only once its instances are gone:
        // the previous driver closes its session first (Retire QUIESCE).
        DriverUpgrade.quiescePrevious(force: false) { print($0) }
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
            print("System extension registered.")
            // The new driver attaches once the previous one is gone.
            if DriverUpgrade.retirePrevious(force: false, timeout: 90, report: { print($0) }) {
                print("Check attachment with `MacLinuxGPUHost ping-bundled`.")
                exitStatus = 0
            } else {
                exitStatus = 3
            }
        case .willCompleteAfterReboot:
            print("System extension activation is pending; attachment has not been verified.")
            DriverUpgrade.resumePrevious { print($0) }
            exitStatus = 2
        @unknown default:
            print("System extension request finished with result: \(result)")
            DriverUpgrade.resumePrevious { print($0) }
            exitStatus = 1
        }
        exit(exitStatus)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        self.request = nil
        fputs("System extension activation failed: \(error)\n", stderr)
        DriverUpgrade.resumePrevious { print($0) }
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
private let kSelHostWindow:       UInt32 = 54
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
private let kSelRetire:           UInt32 = 85 // entitled: hand the GPU to a new driver; see session_state.h
private let kRetireOpQuiesce:   UInt64 = 0
private let kRetireOpTerminate: UInt64 = 1
private let kRetireOpResume:    UInt64 = 2
private let kRetireForce:       UInt64 = 1
private let kRetireConfirm:     UInt64 = 0x52455452 // "RETR"

// Observer clients (dext/sources/session_state.h) read cached state only and
// may attach while a session closes or stays quarantined.
private let kUserClientSession:  UInt32 = 0
private let kUserClientObserver: UInt32 = 1
private let kQuerySessionState:  UInt64 = 0x4c534553
private let kSessionStateWords = 9
private let kIOReturnNotPrivilegedValue = kern_return_t(bitPattern: 0xe00002c1)
private let kIOReturnUnsupportedValue = kern_return_t(bitPattern: 0xe00002c7)
private let kIOReturnNotAttachedValue = kern_return_t(bitPattern: 0xe00002d9)
private let kIOReturnNotFoundValue = kern_return_t(bitPattern: 0xe00002f0)
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
    var retiring: Bool { flags & (1 << 12) != 0 }

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
        if retiring {
            return closing ? "Retiring for a driver upgrade: session closing"
                           : "Retiring for a driver upgrade: no session, new sessions refused"
        }
        return closing ? "Session closing" : "No quarantine (session generation \(generation), \(participants) client(s))"
    }
}

/// The answer to Retire (selector 85; dext/sources/session_state.h).
struct RetireResult {
    let status: kern_return_t
    let state: UInt64
    let detail: UInt64

    static let idle: UInt64 = 0, terminating: UInt64 = 1, closing: UInt64 = 2, clients: UInt64 = 3
    static let rawBAR: UInt64 = 4, quarantined: UInt64 = 5, stopping: UInt64 = 6, resumed: UInt64 = 7

    init?(_ values: [UInt64]) {
        guard values.count == 3 else { return nil }
        status = kern_return_t(bitPattern: UInt32(truncatingIfNeeded: values[0]))
        state = values[1]
        detail = values[2]
    }

    /// The instance stops and its process exits (or already does).
    var leaving: Bool { state == RetireResult.terminating || state == RetireResult.stopping }
    /// Nothing later in this process can change the answer: restart the Mac.
    var restartRequired: Bool { state == RetireResult.quarantined && status == kIOReturnError }

    var summary: String {
        switch state {
        case RetireResult.idle: return "idle: no session, new sessions refused"
        case RetireResult.terminating: return "terminating: its clients and the driver stop, then its process exits"
        case RetireResult.closing: return "closing its session through the normal close"
        case RetireResult.clients: return "\(detail) session client(s) still attached"
        case RetireResult.rawBAR: return "a client still maps a GPU BAR; closing now would quarantine the session"
        case RetireResult.quarantined:
            let blocker = SessionState.name(SessionState.blockers, detail)
            return restartRequired
                ? "quarantined (\(blocker)): restart the Mac, do not kill the driver"
                : "quarantined, release pending (\(blocker))"
        case RetireResult.stopping: return "already stopping"
        case RetireResult.resumed: return "resumed: sessions admitted again"
        default: return "unknown state \(state)"
        }
    }
}

// ----------------------------------------------------------------
// MARK: - Driver instances and upgrades
//
// macOS does not stop a running driver extension when an activation request
// replaces it. kernelmanagerd declines to terminate the old version ("being
// replaced"), sysextd leaves it "terminating for upgrade via delegate", and
// the kernel attaches the new version only once every instance of the old
// one is gone: until then a GPU that appears matches the old version. An
// instance goes when its GPU leaves the bus or when it terminates itself,
// which Retire (selector 85) asks it to do. Killing it is never the way: a
// driver that dies while it holds the PCI device can panic the Mac.
// ----------------------------------------------------------------

/// One MacLinuxGPU driver instance (an IOUserService of this dext).
struct DriverInstance {
    let registryID: UInt64
    /// IOUserServerCDHash: the code directory hash of the running binary.
    let cdhash: String
    /// IOUserClientCreator ("pid N, name") of each attached user client.
    let clients: [String]

    var label: String { String(format: "instance %#llx (cdhash %@)", registryID, String(cdhash.prefix(12))) }
}

enum DriverInstances {
    static func list() -> [DriverInstance] {
        guard let matching = IOServiceMatching("IOUserService") else { return [] }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return []
        }
        defer { IOObjectRelease(iterator) }
        var instances: [DriverInstance] = []
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            defer { IOObjectRelease(service) }
            guard property(service, "CFBundleIdentifier") as? String == dextBundleIdentifier else { continue }
            var registryID: UInt64 = 0
            guard IORegistryEntryGetRegistryEntryID(service, &registryID) == KERN_SUCCESS else { continue }
            let cdhash = (property(service, "IOUserServerCDHash") as? String ?? "").lowercased()
            instances.append(DriverInstance(registryID: registryID, cdhash: cdhash, clients: clients(of: service)))
        }
        return instances
    }

    private static func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    private static func clients(of service: io_service_t) -> [String] {
        var iterator: io_iterator_t = 0
        guard IORegistryEntryGetChildIterator(service, kIOServicePlane, &iterator) == KERN_SUCCESS else {
            return []
        }
        defer { IOObjectRelease(iterator) }
        var creators: [String] = []
        while true {
            let child = IOIteratorNext(iterator)
            if child == 0 { break }
            if let creator = property(child, "IOUserClientCreator") as? String { creators.append(creator) }
            IOObjectRelease(child)
        }
        return creators
    }

    /// The code directory hash IOKit records as IOUserServerCDHash.
    static func cdhash(ofCodeAt url: URL) -> String? {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(url as CFURL, SecCSFlags(), &code) == errSecSuccess,
              let code else { return nil }
        var information: CFDictionary?
        guard SecCodeCopySigningInformation(code, SecCSFlags(), &information) == errSecSuccess,
              let info = information as? [String: Any],
              let unique = info[kSecCodeInfoUnique as String] as? Data else { return nil }
        return unique.map { String(format: "%02x", $0) }.joined()
    }

    /// The driver this app bundles (and installs).
    static func bundledCDHash() -> String? {
        cdhash(ofCodeAt: Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext"))
    }

    /// Instances running another build than the bundled driver.
    static func previous() -> [DriverInstance] {
        guard let bundled = bundledCDHash() else { return [] }
        return list().filter { $0.cdhash != bundled }
    }
}

enum DriverUpgrade {
    /// One Retire call on one instance, over an observer client.
    static func call(_ instance: DriverInstance, _ op: UInt64, force: Bool)
        -> (result: RetireResult?, status: kern_return_t, session: SessionState?) {
        let host = MacLinuxGPUHost()
        host.quiet = true
        guard host.openUserClient(observer: true, registryID: instance.registryID) else {
            return (nil, kIOReturnNotFoundValue, nil)
        }
        let result = host.retire(op, force: force)
        let status = host.lastStatus
        let session = host.sessionState()
        _ = host.closeUserClient()
        return (result, status, session)
    }

    private static func describe(_ instance: DriverInstance) -> String {
        instance.clients.isEmpty ? instance.label
            : instance.label + "; clients: " + instance.clients.joined(separator: "; ")
    }

    /// Retire is refused with kIOReturnNotPrivileged (0xe00002c1) when the
    /// caller lacks the session-release authorization the driver checks.
    static let notPrivilegedAdvice = "refused Retire (kIOReturnNotPrivileged, 0xe00002c1): this app is not " +
        "authorized to end the previous driver's sessions, so it leaves only once every app using it has " +
        "disconnected"

    /// The apps attached to an instance, as \"name (pid N)\", from IOKit's
    /// IOUserClientCreator records (\"pid N, name\").
    static func clientApps(_ instance: DriverInstance) -> [String] {
        instance.clients.compactMap { creator -> String? in
            let parts = creator.split(separator: ",", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2, parts[0].hasPrefix("pid ") else { return creator }
            // This app itself (the installer's own observer) is not a blocker.
            if Int32(parts[0].dropFirst(4)) == getpid() { return nil }
            return "\(parts[1]) (\(parts[0]))"
        }
    }

    /// Ask our own monitoring apps to let go of a previous driver instance:
    /// a Darwin notification whose state is the instance's registry ID. Apps
    /// that follow it (amdgpu_mtopg) close their connection to that instance
    /// and reopen only to the new driver.
    static let retiringNotification = "com.geramyloveless.maclinuxgpu.driver-retiring"
    static func announceRetiring(_ instance: DriverInstance) {
        var token: Int32 = 0
        guard notify_register_check(retiringNotification, &token) == 0 else { return }
        notify_set_state(token, instance.registryID)
        notify_post(retiringNotification)
        notify_cancel(token)
    }

    static let legacyAdvice = "it predates Retire and stays until its GPU leaves the bus or the Mac restarts. " +
        "Do not kill it. Builds 0.1.128 (232) to 0.1.129 (233) leave cleanly when the GPU enclosure is switched off; " +
        "switch it off, wait for the driver to go, then switch it on."

    /// Before the activation request: the previous driver closes its session
    /// the normal way and admits no new one, so nothing runs on the GPU when
    /// the replacement is accepted. Without `force` a session other clients
    /// still use stays open (they are listed); Retire after the activation
    /// then waits for them.
    static func quiescePrevious(force: Bool, report: (String) -> Void) {
        // The display daemon's session would keep the previous driver from
        // closing; it starts again once the new driver is verified.
        if !DriverInstances.previous().isEmpty {
            report("Stopping display agents while the driver is replaced.")
            DisplayAutostart.suspendForUpgrade()
        }
        for instance in DriverInstances.previous() {
            announceRetiring(instance)
            let (result, status, session) = call(instance, kRetireOpQuiesce, force: force)
            guard let result else {
                if status == kIOReturnUnsupportedValue {
                    report("Previous driver \(instance.label): " + legacyAdvice)
                } else if status == kIOReturnNotPrivilegedValue {
                    report("Previous driver \(instance.label) " + notPrivilegedAdvice + ".")
                } else {
                    report(String(format: "Previous driver %@ did not answer Retire (kr=%#x).", instance.label, status))
                }
                continue
            }
            var line = "Previous driver \(describe(instance)): \(result.summary)."
            if result.state == RetireResult.clients {
                line += " Quit those apps, or run `MacLinuxGPUHost retire-previous --force` to close their sessions."
            }
            if let session, result.state == RetireResult.quarantined { line += " " + session.summary + "." }
            report(line)
        }
    }

    /// After macOS accepted the replacement: every previous instance closes
    /// its session and terminates; wait until each is gone. Returns true
    /// when none is left; otherwise reports why and how to recover.
    @discardableResult
    static func retirePrevious(force: Bool, timeout: TimeInterval, report: (String) -> Void) -> Bool {
        if !DriverInstances.previous().isEmpty {
            report("Stopping display agents while the previous driver hands over the GPU.")
            DisplayAutostart.suspendForUpgrade()
        }
        if DriverInstances.bundledCDHash() == nil {
            report("Cannot read the bundled driver's code signature, so a previous driver instance cannot be " +
                   "told apart; `MacLinuxGPUHost instances` lists what is attached.")
            return true
        }
        let deadline = Date().addingTimeInterval(timeout)
        var lastLine: [UInt64: String] = [:]
        var stuck: [UInt64: String] = [:] // nothing later in this boot can make it go
        var announced = false
        while true {
            let previous = DriverInstances.previous()
            if previous.isEmpty {
                if announced { report("The previous driver is gone; the new driver attaches to the GPU.") }
                return true
            }
            if !announced {
                report("Handing the GPU to the new driver: asking \(previous.count) previous driver instance(s) to close and terminate.")
                announced = true
            }
            for instance in previous where stuck[instance.registryID] == nil {
                announceRetiring(instance)
                let (result, status, session) = call(instance, kRetireOpTerminate, force: force)
                var line: String
                if let result {
                    line = "Previous driver \(describe(instance)): \(result.summary)."
                    if result.restartRequired {
                        if let session { line += " " + session.summary + "." }
                        stuck[instance.registryID] = line
                    }
                } else if status == kIOReturnUnsupportedValue {
                    line = "Previous driver \(instance.label): " + legacyAdvice
                    stuck[instance.registryID] = line
                } else if status == kIOReturnNotFoundValue || status == kIOReturnNotAttachedValue {
                    line = "Previous driver \(instance.label) is leaving."
                } else if status == kIOReturnNotPrivilegedValue {
                    line = "Previous driver \(instance.label) " + notPrivilegedAdvice + "."
                } else {
                    line = String(format: "Previous driver %@ did not answer Retire (kr=%#x).", instance.label, status)
                }
                // Whatever keeps it attached, by name: the user can quit it.
                let apps = clientApps(instance)
                if !apps.isEmpty {
                    line += " Still connected: " + apps.joined(separator: ", ") + "; quit " +
                        (apps.count == 1 ? "it" : "them") + " so the previous driver can leave."
                }
                if lastLine[instance.registryID] != line {
                    report(line)
                    lastLine[instance.registryID] = line
                }
            }
            if previous.allSatisfy({ stuck[$0.registryID] != nil }) || Date() >= deadline { break }
            Thread.sleep(forTimeInterval: 0.5)
        }
        let remaining = DriverInstances.previous()
        if remaining.isEmpty { return true }
        report("The previous driver is still attached after \(Int(timeout)) s, so the new driver cannot attach yet. Do not kill it:")
        for instance in remaining {
            report("  " + (stuck[instance.registryID] ?? lastLine[instance.registryID] ?? describe(instance)))
        }
        report("  Quit the apps listed above and run `MacLinuxGPUHost retire-previous` (add --force to close their " +
               "sessions). If it reports a quarantine that needs a restart, or predates Retire, restart the Mac (or " +
               "switch the GPU enclosure off and on for builds 0.1.128 (232) and later); the new driver attaches then.")
        let extensions = InstallPackage.run("/usr/bin/systemextensionsctl", ["list"]).output
            .split(separator: "\n").filter { $0.contains(dextBundleIdentifier) }
        for line in extensions { report("  " + line.trimmingCharacters(in: .whitespaces)) }
        return false
    }

    /// The replacement failed or was deferred: previous instances admit
    /// sessions again.
    static func resumePrevious(report: (String) -> Void) {
        for instance in DriverInstances.previous() {
            if let result = call(instance, kRetireOpResume, force: false).result,
               result.state == RetireResult.resumed {
                report("Previous driver \(instance.label) admits sessions again.")
            }
        }
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
    /// The single-tenant client (type 0), or an observer.  Returns true on
    /// success.
    ///
    /// The dext's Info.plist IOKitPersonalities (the IOPCIDevice provider)
    /// make IOKit instantiate MacLinuxGPU on PCI match; the UserClient is
    /// spawned per IOServiceOpen.  The service is found by the dext's bundle
    /// identifier, never by a PCI device ID: the dext attaches to whichever
    /// AMD GPU matched.  During an upgrade the previous driver version can
    /// still be attached, so the instance running the driver this app
    /// bundles is preferred; `bundledOnly` refuses any other, and
    /// `registryID` names one instance.
    @discardableResult
    func openUserClient(allowUnverified: Bool = false, observer: Bool = false,
                        registryID: UInt64? = nil, bundledOnly: Bool = false) -> Bool {
        if isOpen { return true }
        var candidates = DriverInstances.list()
        if let registryID {
            candidates = candidates.filter { $0.registryID == registryID }
        } else if let bundled = DriverInstances.bundledCDHash() {
            candidates = candidates.filter { $0.cdhash == bundled } +
                (bundledOnly ? [] : candidates.filter { $0.cdhash != bundled })
        } else if bundledOnly {
            append("openUserClient: the bundled driver's code signature could not be read")
            return false
        }
        guard let instance = candidates.first else {
            append("openUserClient: no MacLinuxGPU service found for \(dextBundleIdentifier)"
                   + (bundledOnly ? " running the bundled driver" : ""))
            return false
        }
        guard let matching = IORegistryEntryIDMatching(instance.registryID) else {
            append("openUserClient: IORegistryEntryIDMatching failed")
            return false
        }
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, matching)
        guard svc != 0 else {
            append("openUserClient: the MacLinuxGPU service left the registry")
            return false
        }
        defer { IOObjectRelease(svc) }
        var connection: io_connect_t = 0
        var opened = IOServiceOpen(svc, mach_task_self_,
                                   observer ? kUserClientObserver : kUserClientSession,
                                   &connection)
        if observer && opened == kIOReturnUnsupportedValue {
            // Drivers without observer clients accept only a session client.
            opened = IOServiceOpen(svc, mach_task_self_, kUserClientSession, &connection)
        }
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
        // The product name the dext publishes (libdrm's table), when it has one.
        if let name = readGPUDevices().first(where: { $0.deviceID == Int(did) })?.name {
            text = name + " (" + text + ")"
        }
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
        // Place the host window before the probe, as the HSA runtime's
        // initializeDevice does: query the GART aperture size (selector 54
        // with 0), reserve that much of this process's address space aligned
        // to its size, and hand the base to the driver. It is only a
        // placement hint, released once InitDevice returns.
        var reservation: (UnsafeMutableRawPointer, Int)?
        let (queryKR, query) = callScalar(kSelHostWindow, inScalars: [0], outScalars: 3)
        if queryKR == kIOReturnSuccess, query.count == 3 {
            let bytes = query[1]
            if bytes >= 16384, bytes & (bytes - 1) == 0, bytes <= 1 << 45,
               let raw = mmap(nil, Int(bytes) * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0),
               raw != MAP_FAILED {
                reservation = (raw, Int(bytes) * 2)
                let base = (UInt64(UInt(bitPattern: raw)) + bytes - 1) & ~(bytes - 1)
                let (setKR, set) = callScalar(kSelHostWindow, inScalars: [base], outScalars: 3)
                if setKR == kIOReturnSuccess, set.count == 3, set[0] == base, set[1] == bytes {
                    append(String(format: "initDevice: host window at %#llx, %llu MiB", base, bytes >> 20))
                } else {
                    append(String(format: "initDevice: host window not placed (kr=%#x)", setKR))
                }
            } else {
                append(String(format: "initDevice: cannot reserve a %#llx-byte host window", bytes))
            }
        } else {
            append(String(format: "initDevice: host window query failed (kr=%#x)", queryKR))
        }
        let (kr, _) = callScalar(kSelInitDevice, inScalars: [], outScalars: 0)
        if let (raw, length) = reservation { munmap(raw, length) }
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

    /// Retire (selector 85): hand the GPU to a replacement driver. QUIESCE
    /// closes the session the normal way and refuses new ones; TERMINATE
    /// also has the instance terminated once idle; RESUME undoes QUIESCE.
    /// Requires the session-release entitlement. Nil with `lastStatus` set
    /// when the call failed (Unsupported: a driver older than Retire).
    func retire(_ op: UInt64, force: Bool = false) -> RetireResult? {
        let (kr, values) = callScalar(kSelRetire, inScalars: [op, force ? kRetireForce : 0, kRetireConfirm],
                                      outScalars: 3)
        lastStatus = kr
        guard kr == kIOReturnSuccess, let result = RetireResult(values) else {
            append(String(format: "retire: refused (kr=%#x)", kr))
            return nil
        }
        append("retire: " + result.summary)
        return result
    }
    private(set) var lastStatus: kern_return_t = kIOReturnSuccess

    // MARK: Logging

    /// No console output (polling loops report their own summaries).
    var quiet = false

    private func append(_ fmt: String, _ args: CVarArg...) {
        let line = String(format: fmt, arguments: args)
        logLines.append(line)
        if !quiet { print("[mac.linuxgpu.host] " + line) }
    }

    var log: [String] { return logLines }
}

// ----------------------------------------------------------------
// MARK: - The display test (selector 84, dext/sources/session_state.h)
//
// The driver's in-kernel DRM client (linuxu/headers/rt/display.h) probes
// every connector Display Core found and shows a static pattern on the
// connected outputs through upstream KMS. Observer client only; the GPU
// must be running (display-test --init brings it up with a session client
// held for the run).
// ----------------------------------------------------------------
let kSelSysfsRead: UInt32 = 80
let kSelDisplay: UInt32 = 84
let kDisplayConfirm: UInt64 = 0x44495350   // "DISP"
let kDisplayReportMax = 1024

extension MacLinuxGPUHost {
    /// IOConnectCallMethod with scalars and structures both ways.
    func callMethod(_ selector: UInt32, inScalars: [UInt64], inData: Data,
                    outScalars: Int, outSize: Int) -> (kern_return_t, [UInt64], Data) {
        guard isOpen else { return (kIOReturnError, [], Data()) }
        var outBuf = [UInt64](repeating: 0, count: max(1, outScalars))
        var outN = UInt32(outBuf.count)
        var outData = Data(count: max(1, outSize))
        var outCnt = size_t(outSize)
        let kr: kern_return_t = inScalars.withUnsafeBufferPointer { ibuf in
            outBuf.withUnsafeMutableBufferPointer { obuf in
                outData.withUnsafeMutableBytes { optr in
                    inData.withUnsafeBytes { iptr in
                        IOConnectCallMethod(ucConn, selector, ibuf.baseAddress, UInt32(inScalars.count),
                                            inData.isEmpty ? nil : iptr.baseAddress, inData.count,
                                            obuf.baseAddress, &outN, optr.baseAddress, &outCnt)
                    }
                }
            }
        }
        return (kr, Array(outBuf.prefix(Int(outN))), outData.prefix(Int(min(outCnt, size_t(outSize)))))
    }

    /// One display op: (IOReturn, Linux status, report).
    func display(_ op: DisplayOp, pattern: UInt64 = 0, connector: String? = nil)
        -> (kern_return_t, Int64, DisplayReport?) {
        let name = Data((connector ?? "").utf8)
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [op.rawValue, pattern, kDisplayConfirm],
                                            inData: name, outScalars: 2, outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), DisplayReport(data))
    }

    /// One connector's probed modes (op MODES).
    func displayModes(_ connector: String) -> (kern_return_t, Int64, DisplayModes?) {
        let (kr, values, data) = callMethod(kSelDisplay, inScalars: [DisplayOp.modes.rawValue, 0, kDisplayConfirm],
                                            inData: Data(connector.utf8), outScalars: 2, outSize: kDisplayReportMax)
        guard kr == kIOReturnSuccess, let status = values.first else { return (kr, 0, nil) }
        return (kr, Int64(bitPattern: status), DisplayModes(data))
    }

    /// A sysfs file under the device directory (selector 80), read whole.
    func sysfsRead(_ path: String, list: Bool = false) -> (Int64, Data)? {
        var data = Data()
        while true {
            let (kr, values, chunk) = callMethod(kSelSysfsRead, inScalars: [list ? 1 : 0, UInt64(data.count)],
                                                 inData: Data(path.utf8), outScalars: 3, outSize: 4096)
            guard kr == kIOReturnSuccess, values.count == 3 else { return nil }
            let status = Int64(bitPattern: values[0])
            if status != 0 { return (status, Data()) }
            data.append(chunk.prefix(Int(values[1])))
            let length = values[2]
            if values[1] == 0 || (length > 0 && UInt64(data.count) >= length) || (length == 0 && values[1] < 4096) {
                return (0, data)
            }
        }
    }

    /// The connector's EDID, as Linux shows /sys/class/drm/card0-<name>/edid.
    func connectorEDID(_ name: String) -> Data? {
        guard let (status, listing) = sysfsRead("drm", list: true), status == 0 else { return nil }
        let cards = String(decoding: listing, as: UTF8.self).split(separator: "\n")
            .filter { $0.hasPrefix("d card") && !$0.contains("-") }.map { String($0.dropFirst(2)) }
        for card in cards.sorted() {
            if let (status, edid) = sysfsRead("drm/\(card)/\(card)-\(name)/edid"), status == 0 { return edid }
        }
        return nil
    }
}

/// display-agent: follow the GPU's connected monitors and maintain a macOS
/// virtual display for each (docs/macos-displays.md). Polls the driver's
/// cached connector state (STATUS) and, when the hotplug epoch moves,
/// probes, reads each connected monitor's modes and EDID and plans its
/// CGVirtualDisplay. Only --dry-run exists so far: it prints the plans and
/// the changes and creates nothing.
func runDisplayAgent(_ options: [String]) -> Int32 {
    func value(_ flag: String) -> String? {
        guard let at = options.firstIndex(of: flag), at + 1 < options.count else { return nil }
        return options[at + 1]
    }
    if options.contains("--create") { return runDisplayAgentCreate(options) }
    guard options.contains("--dry-run") else {
        print("display-agent: say --dry-run (print what it would create) or --create (make a virtual display)")
        return 2
    }
    let once = options.contains("--once")
    let interval = max(50, Int(value("--interval") ?? "") ?? 500)
    var session: MacLinuxGPUHost?
    if options.contains("--init") {
        let host = MacLinuxGPUHost()
        guard host.openUserClient(), host.initDevice() else {
            print("ERROR: GPU initialization failed (scripts/read-driver-log.py shows why)")
            return 1
        }
        session = host
    }
    defer { session?.closeUserClient() }
    let observer = MacLinuxGPUHost()
    guard observer.openUserClient(observer: true) else {
        print("ERROR: failed to open an observer client")
        return 1
    }
    defer { observer.closeUserClient() }

    var state = DisplayAgentState()
    var failures = 0
    let notPermitted = kern_return_t(bitPattern: 0xe00002e2)
    func failure(_ kr: kern_return_t, _ what: String) -> String {
        kr == kern_return_t(bitPattern: 0xe00002d8) ?
            "the GPU is not running in an open session (use --init)" :
        kr == notPermitted ?
            "\(what) is not in this driver build (selector 84 ops STATUS and MODES arrived in build 231)" :
            String(format: "%@ failed (kr=%#x)", what, kr)
    }
    while true {
        // A single pass probes at once; following needs the hotplug epoch.
        var probeNow = once
        if !once {
            let (kr, status, report) = observer.display(.status)
            guard kr == kIOReturnSuccess, status == 0, let report else {
                print("display-agent: " + (kr == kIOReturnSuccess ? "status: \(displayErrno(status))" :
                                           failure(kr, "STATUS")))
                return 1
            }
            probeNow = state.needsProbe(epoch: report.hotplugEpoch)
            if probeNow { print("display-agent: hotplug epoch \(report.hotplugEpoch): probing") }
        }
        if probeNow {
            let (pkr, pstatus, probed) = observer.display(.probe)
            guard pkr == kIOReturnSuccess, let probed else {
                print("display-agent: " + failure(pkr, "PROBE"))
                return 1
            }
            if pstatus != 0 { print("display-agent: probe: \(displayErrno(pstatus))") }
            var plans: [VirtualDisplayPlan] = []
            for connector in probed.connectors where connector.connected {
                let (mkr, mstatus, modes) = observer.displayModes(connector.name)
                guard mkr == kIOReturnSuccess, mstatus == 0, let modes else {
                    print("  \(connector.name): no plan: " + (mkr == kIOReturnSuccess ?
                          "modes: \(displayErrno(mstatus))" : failure(mkr, "MODES")))
                    if let edid = observer.connectorEDID(connector.name), let info = EDIDSummary(edid) {
                        print("  \(connector.name) EDID (\(edid.count) bytes): \(info.summary)")
                    }
                    failures += 1
                    continue
                }
                switch planVirtualDisplay(connector: connector.name, modes: modes,
                                          edid: observer.connectorEDID(connector.name)) {
                case .success(let plan):
                    plans.append(plan)
                case .failure(let error):
                    print("  \(connector.name): no virtual display: \(error)")
                    failures += 1
                }
            }
            for change in state.apply(plans) {
                print("display-agent: would \(change)")
                switch change {
                case .add(let plan), .update(let plan): plan.lines.forEach { print($0) }
                default: break
                }
            }
            if plans.isEmpty { print("display-agent: no connected monitor to mirror") }
        }
        if once { return failures == 0 ? 0 : 1 }
        Thread.sleep(forTimeInterval: Double(interval) / 1000)
    }
}

/// display-probe / display-test / display-off. Returns the exit status.
func runDisplayCommand(_ command: String, _ options: [String]) -> Int32 {
    func value(_ flag: String) -> String? {
        guard let at = options.firstIndex(of: flag), at + 1 < options.count else { return nil }
        return options[at + 1]
    }
    let initGPU = options.contains("--init")
    let connector = value("--connector")
    let patternName = value("--pattern") ?? "bars"
    guard let pattern = displayPatterns[patternName] else {
        print("unknown pattern \(patternName) (bars, white, gradient)")
        return 2
    }
    var seconds = Double(value("--seconds") ?? "") ?? 0
    if command == "display-test" && initGPU && seconds == 0 { seconds = 30 }

    // --init: a session client brings the GPU up (InitDevice with the
    // firmware servicer) and stays open for the run.
    var session: MacLinuxGPUHost?
    if initGPU {
        let host = MacLinuxGPUHost()
        guard host.openUserClient(), host.initDevice() else {
            print("ERROR: GPU initialization failed (scripts/read-driver-log.py shows why)")
            return 1
        }
        session = host
    }
    defer { session?.closeUserClient() }
    let observer = MacLinuxGPUHost()
    guard observer.openUserClient(observer: true) else {
        print("ERROR: failed to open an observer client")
        return 1
    }
    defer { observer.closeUserClient() }

    func perform(_ op: DisplayOp, _ label: String, pattern: UInt64 = 0, connector: String? = nil) -> DisplayReport? {
        let (kr, status, report) = observer.display(op, pattern: pattern, connector: connector)
        if kr != kIOReturnSuccess {
            let reason = kr == kern_return_t(bitPattern: 0xe00002d8) ? "not ready: the GPU is not running in an open session (use --init)"
                : kr == kern_return_t(bitPattern: 0xe00002e2) ? "not permitted: this driver predates the display test"
                : kr == kern_return_t(bitPattern: 0xe00002d5) ? "busy: another display operation is running"
                : String(format: "call failed (kr=%#x)", kr)
            print("\(label): \(reason)")
            return nil
        }
        print("\(label): \(displayErrno(status))")
        guard let report else { print("  (malformed report)"); return nil }
        if status != 0 && op == .show {
            print("  probe \(displayErrno(Int64(report.probeStatus))), commit \(displayErrno(Int64(report.commitStatus))), restore \(displayErrno(Int64(report.restoreStatus)))")
        }
        report.lines.forEach { print($0) }
        return status == 0 ? report : nil
    }

    switch command {
    case "display-probe":
        guard let report = perform(.probe, "probe") else { return 1 }
        var failed = false
        if options.contains("--edid") {
            for c in report.connectors where c.status == 1 {
                guard let edid = observer.connectorEDID(c.name) else {
                    print("  \(c.name) EDID: unreadable"); failed = true; continue
                }
                print("  \(c.name) EDID (\(edid.count) bytes): \(EDIDSummary(edid)?.summary ?? "not a valid EDID base block")")
                for at in stride(from: 0, to: edid.count, by: 16) {
                    let row = edid[at..<min(at + 16, edid.count)].map { String(format: "%02x", $0) }
                    print(String(format: "    %04x  ", at) + row.joined(separator: " "))
                }
            }
        }
        return failed ? 1 : 0
    case "display-off":
        return perform(.off, "off") == nil ? 1 : 0
    default:
        guard perform(.show, "show \(patternName) on \(connector ?? "every connected output")",
                      pattern: pattern, connector: connector) != nil else { return 1 }
        guard seconds > 0 else { return 0 }
        Thread.sleep(forTimeInterval: seconds)
        return perform(.off, "off after \(seconds) s") == nil ? 1 : 0
    }
}

/// The GPUs the dext drives, as its services publish them in the IORegistry
/// (host/DeviceInfo.swift). Needs no UserClient and joins no session.
func readGPUDevices() -> [GPUDeviceInfo] {
    guard let matching = IOServiceMatching("IOUserService") else { return [] }
    var iter: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, matching as CFDictionary, &iter)
            == kIOReturnSuccess else { return [] }
    defer { IOObjectRelease(iter) }
    var devices: [GPUDeviceInfo] = []
    var svc = IOIteratorNext(iter)
    while svc != 0 {
        var props: Unmanaged<CFMutableDictionary>?
        if IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0) == kIOReturnSuccess,
           let dict = props?.takeRetainedValue() as? [String: Any],
           dict["CFBundleIdentifier"] as? String == dextBundleIdentifier,
           let device = GPUDeviceInfo(properties: dict) {
            devices.append(device)
        }
        IOObjectRelease(svc)
        svc = IOIteratorNext(iter)
    }
    return devices
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
    @Published var displayAutostart = DisplayAutostart.isEnabled
    @Published var menuBarShown = MenuBarAgent.isEnabled
    /// The GPU and its monitors (readGPUDevices), refreshed while the app runs.
    @Published var gpuSummary = ""

    private var request: OSSystemExtensionRequest?
    private var gpuTimer: Timer?
    private var checkingProperties = false
    private var hasStarted = false

    func start() {
        guard !hasStarted else { return }
        hasStarted = true
        refreshGPU()
        gpuTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshGPU() }
        }
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
            // An earlier upgrade may have left the previous driver attached.
            retireThenVerify()
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
        // macOS replaces a running driver only once its instances are gone.
        // The previous driver closes its session first (Retire QUIESCE);
        // after the replacement is accepted it terminates (retireThenVerify).
        self.request = nil
        status = "Preparing the running driver…"
        Task { [weak self] in
            await Task.detached(priority: .userInitiated) { [weak self] in
                DriverUpgrade.quiescePrevious(force: false) { line in
                    Task { @MainActor [weak self] in self?.append(line) }
                }
            }.value
            self?.requestActivation()
        }
    }

    private func requestActivation() {
        status = "Requesting activation…"
        append("Requesting activation of \(dextBundleIdentifier) \(bundledVersion).")
        let activation = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        self.request = activation
        activation.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activation)
    }

    /// The new driver attaches only once every previous instance is gone:
    /// Retire TERMINATE each one, wait, then verify the bundled driver.
    private func retireThenVerify() {
        isWorking = true
        status = "Handing the GPU to the new driver…"
        runningStatus = "waiting for the previous driver"
        Task { [weak self] in
            let retired = await Task.detached(priority: .userInitiated) { [weak self] () -> Bool in
                DriverUpgrade.retirePrevious(force: false, timeout: 90) { line in
                    Task { @MainActor [weak self] in self?.append(line) }
                }
            }.value
            guard let self else { return }
            if retired {
                self.verifyAttachedDriver()
            } else {
                self.status = "Previous driver still attached"
                self.runningStatus = "previous driver attached; see the log"
                self.isWorking = false
            }
        }
    }

    private func resumePrevious() {
        Task.detached(priority: .utility) { [weak self] in
            DriverUpgrade.resumePrevious { line in
                Task { @MainActor [weak self] in self?.append(line) }
            }
        }
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
            retireThenVerify()
        case .willCompleteAfterReboot:
            status = "Activation pending"
            runningStatus = "replacement staged"
            append("macOS deferred activation of this extension; attachment has not been verified.")
            resumePrevious()
            isWorking = false
        @unknown default:
            status = "Unknown activation result"
            append("macOS returned an unknown extension result: \(result.rawValue)")
            resumePrevious()
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
        resumePrevious()
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
                    // reports why and whether killing it would be unsafe. Only
                    // the bundled driver counts: a previous one still attached
                    // would answer as well.
                    let observer = MacLinuxGPUHost()
                    if observer.openUserClient(observer: true, bundledOnly: true) {
                        let state = observer.sessionState()
                        _ = observer.closeUserClient()
                        if let state, state.quarantined {
                            return (false, state.summary)
                        }
                    }
                    if driver.openUserClient(bundledOnly: true) {
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
                // Monitors on the GPU become Mac displays from now on, unless
                // the user turned that off.
                if let error = DisplayAutostart.enableUnlessDeclined() {
                    self.append("Display autostart: \(error)")
                } else if DisplayAutostart.isEnabled {
                    self.append("Display autostart is on: monitors on the GPU appear as Mac displays (log: \(DisplayAutostart.logURL.path)).")
                }
                self.displayAutostart = DisplayAutostart.isEnabled
                if let error = MenuBarAgent.enableUnlessHidden() {
                    self.append("Menu bar item: \(error)")
                }
                self.menuBarShown = MenuBarAgent.isEnabled
            } else {
                self.status = "Driver not attached"
                self.append(result + ". Registration alone does not show that the dext is running.")
            }
            self.isWorking = false
        }
    }

    func setMenuBarShown(_ on: Bool) {
        if let error = on ? MenuBarAgent.enable() : MenuBarAgent.disable() { append("Menu bar item: \(error)") }
        menuBarShown = MenuBarAgent.isEnabled
    }

    func setDisplayAutostart(_ on: Bool) {
        let error = on ? DisplayAutostart.enable() : DisplayAutostart.disable()
        if let error { append("Display autostart: \(error)") }
        else { append("Display autostart " + (on ? "on: the agent starts now and at every login." : "off.")) }
        displayAutostart = DisplayAutostart.isEnabled
    }

    func refreshGPU() {
        let summary = readGPUDevices().map { $0.summaryLines.joined(separator: "\n") }
            .joined(separator: "\n\n")
        if summary != gpuSummary { gpuSummary = summary }
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
            Toggle("Show monitors on the GPU as Mac displays automatically", isOn: Binding(
                get: { controller.displayAutostart },
                set: { controller.setDisplayAutostart($0) }))
                .disabled(controller.needsCopy || controller.isWorking)
            Toggle("Show MacLinuxGPU in the menu bar", isOn: Binding(
                get: { controller.menuBarShown },
                set: { controller.setMenuBarShown($0) }))
                .disabled(controller.needsCopy || controller.isWorking)
            if !controller.gpuSummary.isEmpty {
                GroupBox("GPU") {
                    Text(controller.gpuSummary)
                        .font(.system(.callout, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
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

        // Driver upgrades (DriverUpgrade): the instances attached, and the
        // safe way to make a previous driver let go of the GPU (never kill).
        if args[1] == "instances" {
            let bundled = DriverInstances.bundledCDHash()
            let instances = DriverInstances.list()
            if instances.isEmpty { print("No MacLinuxGPU driver instance is attached.") }
            for instance in instances {
                let role = bundled == nil ? "" : (instance.cdhash == bundled ? " [bundled]" : " [previous]")
                print(instance.label + role)
                for client in instance.clients { print("  client: " + client) }
                let observer = MacLinuxGPUHost()
                observer.quiet = true
                if observer.openUserClient(observer: true, registryID: instance.registryID) {
                    if let state = observer.sessionState() { print("  " + state.summary) }
                    _ = observer.closeUserClient()
                }
            }
            exit(0)
        }
        if args[1] == "retire-previous" || args[1] == "resume-previous" {
            let options = Array(args.dropFirst(2))
            if args[1] == "resume-previous" {
                DriverUpgrade.resumePrevious { print($0) }
                exit(0)
            }
            var wait: TimeInterval = 90
            if let index = options.firstIndex(of: "--wait"), index + 1 < options.count,
               let seconds = TimeInterval(options[index + 1]), seconds >= 0 {
                wait = seconds
            }
            if DriverInstances.previous().isEmpty {
                print("No previous driver instance is attached.")
                exit(0)
            }
            let retired = DriverUpgrade.retirePrevious(force: options.contains("--force"), timeout: wait) { print($0) }
            if retired { DisplayAutostart.resumeAfterUpgrade() }
            exit(retired ? 0 : 3)
        }
        if args[1] == "device" {
            // The GPU and the monitors on its outputs, from the IORegistry:
            // what System Information shows, plus the displays it cannot.
            let devices = readGPUDevices()
            if devices.isEmpty {
                print("No MacLinuxGPU service publishes a GPU (is the driver attached?)")
                exit(1)
            }
            print(devices.map { $0.summaryLines.joined(separator: "\n") }.joined(separator: "\n\n"))
            exit(0)
        }
        if args[1] == "display-pin-test" {
            exit(runDisplayPinTest(Array(args.dropFirst(2))))
        }
        if args[1] == "display-agent" {
            exit(runDisplayAgent(Array(args.dropFirst(2))))
        }
        if args[1] == "display-autostart" {
            exit(runDisplayAutostart(Array(args.dropFirst(2))))
        }
        if args[1] == "menu-bar" {
            exit(runMenuBar())
        }
        if ["display-probe", "display-test", "display-off"].contains(args[1]) {
            exit(runDisplayCommand(args[1], Array(args.dropFirst(2))))
        }

        let host = MacLinuxGPUHost()
        // Cached-state commands use an observer client, which works while a
        // session closes or stays quarantined and never joins a session.
        let observerCommands: Set<String> = ["status", "session", "release", "power", "power-watch",
                                             "power-hold", "pcie"]

        if !host.openUserClient(observer: observerCommands.contains(args[1]),
                                bundledOnly: args[1] == "ping-bundled") {
            print("ERROR: failed to open the UserClient (is the dext activated, "
                  + "signed, and an AMD GPU attached over Thunderbolt?)")
            exit(1)
        }

        let commandStatus: Int32
        switch args[1] {
case "pcie":
    // The GPU's PCI Express settings, from its configuration space as
    // Linux shows it (the device's sysfs "config"); read only.
    if let (status, config) = host.sysfsRead("config"), status == 0, let cap = PCIeCapability(config: config) {
        cap.lines.forEach { print($0) }
        commandStatus = 0
    } else if let (status, config) = host.sysfsRead("config") {
        print(status != 0 ? "ERROR: reading the configuration space: Linux errno \(-status)" :
              "ERROR: no PCI Express capability in \(config.count) bytes of configuration space")
        commandStatus = 1
    } else {
        print("ERROR: the driver did not answer the sysfs read (is the GPU initialized?)")
        commandStatus = 1
    }
case "ping", "ping-bundled":
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
case "power-hold":
    // Holds a low-power request for N seconds (default 10), then drops it:
    // the quiesce and resume a backgrounded app gets, while clients run.
    let seconds = args.count >= 3 ? max(1, Int(args[2]) ?? 10) : 10
    if let state = host.power(kPowerOpPrepare) {
        print("held: " + state.summary)
        Thread.sleep(forTimeInterval: TimeInterval(seconds))
        if let resumed = host.power(kPowerOpResume) { print("released: " + resumed.summary) }
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
