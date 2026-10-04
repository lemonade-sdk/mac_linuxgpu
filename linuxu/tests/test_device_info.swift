// The host's view of the GPU and its monitors (host/DeviceInfo.swift) from
// the properties the dext publishes on its IOService
// (dext/sources/device_properties.h), as IORegistryEntryCreateCFProperties
// returns them. No IOKit; compiled with DeviceInfo.swift by
// scripts/test-device-info.sh.
import Foundation

func check(_ condition: Bool, _ message: String = "", file: String = #file, line: Int = #line) {
    if !condition {
        FileHandle.standardError.write("\(file):\(line): check failed \(message)\n".data(using: .utf8)!)
        exit(1)
    }
}

let device: [String: Any] = [
    "Name": "AMD Radeon AI Pro R9700", "NameSource": "amdgpu.ids",
    "VendorID": NSNumber(value: 0x1002), "DeviceID": NSNumber(value: 0x7551),
    "RevisionID": NSNumber(value: 0xc0), "SubsystemVendorID": NSNumber(value: 0x1da2),
    "SubsystemID": NSNumber(value: 0xe499),
    "PCIeLinkGeneration": NSNumber(value: 5), "PCIeLinkWidth": NSNumber(value: 16),
    "PCIeLinkSpeed": "32.0 GT/s", "DriverProbed": true,
    "GCVersion": "12.0.1", "GFXTarget": "gfx1201",
    "VRAMBytes": NSNumber(value: UInt64(32) << 30), "VisibleVRAMBytes": NSNumber(value: UInt64(256) << 20),
    "VRAMType": "GDDR6", "VRAMBitWidth": NSNumber(value: 256), "ComputeUnits": NSNumber(value: 64),
    "VBIOSPartNumber": "113-EXAMPLE-PN", "VBIOSVersion": "022.001.002.008.000001",
]
let displays: [String: Any] = [
    "HotplugEpoch": NSNumber(value: 3),
    "Connectors": [
        ["Name": "DP-4", "Status": "connected", "Monitor": "DELL UP2716D",
         "WidthMM": NSNumber(value: 597), "HeightMM": NSNumber(value: 336),
         "PreferredWidth": NSNumber(value: 2560), "PreferredHeight": NSNumber(value: 1440),
         "PreferredRefresh": NSNumber(value: 60), "Lit": true,
         "LitWidth": NSNumber(value: 2560), "LitHeight": NSNumber(value: 1440),
         "LitRefresh": NSNumber(value: 60)] as [String: Any],
        ["Name": "DP-5", "Status": "connected", "Monitor": "TEST PANEL",
         "PreferredWidth": NSNumber(value: 1920), "PreferredHeight": NSNumber(value: 1080),
         "PreferredRefresh": NSNumber(value: 60), "Lit": false] as [String: Any],
        ["Name": "HDMI-A-1", "Status": "disconnected", "Lit": false] as [String: Any],
    ],
]
let service: [String: Any] = [
    "CFBundleIdentifier": "com.geramyloveless.MacAMDGPUHost.MacAMDGPU",
    "model": "AMD Radeon AI Pro R9700", "VRAM,totalMB": NSNumber(value: 32768),
    "MacLinuxGPUDevice": device, "MacLinuxGPUDisplays": displays,
]

guard let gpu = GPUDeviceInfo(properties: service) else { check(false, "decode"); exit(1) }
check(gpu.name == "AMD Radeon AI Pro R9700" && gpu.title == "AMD Radeon AI Pro R9700")
check(gpu.vendorID == 0x1002 && gpu.deviceID == 0x7551 && gpu.revisionID == 0xc0)
check(gpu.vramBytes == UInt64(32) << 30 && gpu.vramType == "GDDR6" && gpu.computeUnits == 64)
check(gpu.driverProbed && gpu.gfxTarget == "gfx1201" && gpu.hotplugEpoch == 3)
check(gpu.connectors?.count == 3)
check(gpu.connectors?[0].monitor == "DELL UP2716D" && gpu.connectors?[0].litMode?.width == 2560)
check(gpu.connectors?[1].litMode == nil && gpu.connectors?[1].preferred?.height == 1080)
check(gpu.connectors?[2].connected == false && gpu.connectors?[2].monitor == nil)

let lines = gpu.summaryLines
let text = lines.joined(separator: "\n")
print(text)
check(lines.first == "AMD Radeon AI Pro R9700:")
check(lines.contains("  Type: External GPU"))
check(lines.contains("  PCIe Link: 32.0 GT/s x16"))
check(lines.contains("  VRAM (Total): 32 GB GDDR6 (256-bit)"))
check(lines.contains("  ISA Target: gfx1201") && lines.contains("  VBIOS Version: 113-EXAMPLE-PN"))
check(lines.contains("  PCI ID: 1002:7551 rev c0, subsystem 1da2:e499"))
check(lines.contains("    DELL UP2716D on DP-4:"))
check(lines.contains("      Resolution: 2560 x 1440 @ 60 Hz (driven by this GPU)"))
check(lines.contains("      Size: 597 x 336 mm"))
check(lines.contains("    TEST PANEL on DP-5:") && lines.contains("      Preferred: 1920 x 1080 @ 60 Hz (not driven)"))
check(lines.contains("    Unused outputs: HDMI-A-1"))
check(!text.contains("has not probed"))

// Before the probe: the PCI identity and name only, no displays.
var early = service
early["MacLinuxGPUDisplays"] = nil
early["MacLinuxGPUDevice"] = ["Name": "AMD Radeon AI Pro R9700", "VendorID": NSNumber(value: 0x1002),
                              "DeviceID": NSNumber(value: 0x7551), "DriverProbed": false] as [String: Any]
guard let attached = GPUDeviceInfo(properties: early) else { check(false, "decode early"); exit(1) }
check(attached.connectors == nil && attached.vramBytes == nil)
check(attached.summaryLines.contains("  Driver: attached; the upstream driver has not probed the GPU yet"))
check(!attached.summaryLines.contains { $0.hasPrefix("  VRAM") || $0.hasPrefix("  Displays") })

// No product name: the PCI ID is the title.
early["MacLinuxGPUDevice"] = ["VendorID": NSNumber(value: 0x1002), "DeviceID": NSNumber(value: 0x7480)]
check(GPUDeviceInfo(properties: early)?.title == "AMD GPU 1002:7480")

// A driver without these properties.
check(GPUDeviceInfo(properties: ["CFBundleIdentifier": "x"]) == nil)
check(GPUDeviceInfo.memoryText(UInt64(16) << 30) == "16 GB" && GPUDeviceInfo.memoryText(UInt64(1536) << 20) == "1536 MB")

print("PASS device-info: the GPU with the monitors on its outputs, from the dext's properties")

// The PCI Express capability from a configuration space: the R9700's
// capabilities (DevCap 0x10008fa1, DevCap2 0x730a9f) at reset-like settings.
do {
    var b = [UInt8](repeating: 0, count: 4096)
    func put16(_ at: Int, _ v: UInt16) { b[at] = UInt8(v & 0xff); b[at + 1] = UInt8(v >> 8) }
    func put32(_ at: Int, _ v: UInt32) { put16(at, UInt16(v & 0xffff)); put16(at + 2, UInt16(v >> 16)) }
    b[6] = 0x10; b[0x34] = 0x48
    b[0x48] = 0x01; b[0x49] = 0x64          // power management, next 0x64
    b[0x64] = 0x10; b[0x65] = 0x00          // PCI Express
    put32(0x64 + 4, 0x10008fa1)             // MPS 256 supported, extended tags supported
    put16(0x64 + 8, 0x2010)                 // MRRS 512, MPS 128, relaxed ordering, extended tags off
    put32(0x64 + 12, 0x00000105)            // link capable 32 GT/s x16
    put16(0x64 + 18, 0x0044)                // link at 16 GT/s x4
    put32(0x64 + 36, 0x00730a9f)            // 10-bit tags completer and requester
    put16(0x64 + 40, 0x0000)
    guard let cap = PCIeCapability(config: Data(b)) else { print("no capability"); exit(1) }
    precondition(cap.offset == 0x64)
    precondition(cap.maxPayloadSupported == 256 && cap.maxPayload == 128 && cap.maxReadRequest == 512)
    precondition(cap.extendedTagSupported && !cap.extendedTagEnabled && cap.relaxedOrdering && !cap.noSnoop)
    precondition(cap.tag10Requester && cap.tag10Completer && !cap.tag10Enabled)
    precondition(cap.linkSpeed == 4 && cap.linkWidth == 4 && cap.maxLinkSpeed == 5 && cap.maxLinkWidth == 16)
    precondition(cap.lines[1] == "  link: 16.0 GT/s x4 (the device can do 32.0 GT/s x16)", cap.lines[1])
    precondition(cap.lines[2] == "  max payload: 128 bytes (supports 256)")
    // No capabilities list, a list that loops, a short space: nil.
    var none = b; none[6] = 0
    precondition(PCIeCapability(config: Data(none)) == nil)
    var loop = b; loop[0x49] = 0x48; loop[0x48] = 0x01
    precondition(PCIeCapability(config: Data(loop)) == nil)
    precondition(PCIeCapability(config: Data(b.prefix(0x64 + 20))) == nil)
    print("PASS PCIe capability: list walk, payload, read request, tags, link, refusals")
}
