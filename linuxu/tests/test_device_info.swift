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

// The Resizable BAR capability from a configuration space: the R9700's, as
// read on the Thunderbolt 5 eGPU (VSEC 0x100 -> AER 0x150 -> ReBAR 0x200
// -> power budget 0x240): BAR0 256 MB of 256 MB-32 GB, BAR2 2 MB of 2-256 MB.
do {
    var b = [UInt8](repeating: 0, count: 4096)
    func put32(_ at: Int, _ v: UInt32) { for i in 0..<4 { b[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff) } }
    put32(0x100, 0x1501000b)
    put32(0x150, 0x20020001)
    put32(0x200, 0x24010015)
    put32(0x204, 0x000ff000); put32(0x208, 0x00000840)
    put32(0x20c, 0x00001fe0); put32(0x210, 0x00000102)
    put32(0x240, 0x00010004)
    guard let rebar = ResizableBARCapability(config: Data(b)) else { print("no Resizable BAR"); exit(1) }
    precondition(rebar.offset == 0x200 && rebar.entries.count == 2)
    let bar0 = rebar.entry(bar: 0)!, bar2 = rebar.entry(bar: 2)!
    precondition(bar0.currentBytes == 256 << 20 && bar0.largestBytes == UInt64(32) << 30)
    precondition(bar0.supportedBytes.count == 8 && bar0.supportedBytes.first == 256 << 20)
    precondition(bar2.currentBytes == 2 << 20 && bar2.largestBytes == 256 << 20)
    precondition(rebar.entry(bar: 1) == nil)
    precondition(rebar.lines == [
        "Resizable BAR capability at 0x200:",
        "  BAR0: 256 MB now (supports 256 MB, 512 MB, 1 GB, 2 GB, 4 GB, 8 GB, 16 GB, 32 GB)",
        "  BAR2: 2 MB now (supports 2 MB, 4 MB, 8 MB, 16 MB, 32 MB, 64 MB, 128 MB, 256 MB)",
    ], rebar.lines.joined(separator: "\n"))
    // Refusals: no capability, a looping list, a list leaving the space, a
    // second version, a size not supported, the same BAR twice, no entries,
    // a short space.
    var none = b; none[0x152] = 0x02; none[0x153] = 0x00      // AER ends the list
    precondition(ResizableBARCapability(config: Data(none)) == nil)
    var loop = b; loop[0x152] = 0x02; loop[0x153] = 0x15      // AER's next -> 0x150
    precondition(ResizableBARCapability(config: Data(loop)) == nil)
    var wild = b; wild[0x152] = 0x02; wild[0x153] = 0x02      // AER's next -> 0x020
    precondition(ResizableBARCapability(config: Data(wild)) == nil)
    var v2 = b; v2[0x202] = 0x02
    precondition(ResizableBARCapability(config: Data(v2)) == nil)
    var unsupported = b; unsupported[0x209] = 7               // BAR0 at 128 MB
    precondition(ResizableBARCapability(config: Data(unsupported)) == nil)
    var twice = b; twice[0x210] = 0x00                        // second entry names BAR0
    precondition(ResizableBARCapability(config: Data(twice)) == nil)
    var empty = b; empty[0x208] = 0x00
    precondition(ResizableBARCapability(config: Data(empty)) == nil)
    precondition(ResizableBARCapability(config: Data(b.prefix(0x20c))) == nil)
    precondition(ResizableBARCapability(config: Data(b.prefix(256))) == nil)
    print("PASS Resizable BAR capability: list walk, sizes, refusals")
}
