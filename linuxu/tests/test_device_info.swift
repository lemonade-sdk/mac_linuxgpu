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
