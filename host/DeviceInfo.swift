//
//  DeviceInfo.swift — the GPU and the monitors on its outputs, as the dext
//  publishes them on its IOService (dext/sources/device_identity.h):
//  MacLinuxGPUDevice and MacLinuxGPUDisplays, plus the keys System
//  Information reads (model, VRAM,totalMB). Reading them needs no
//  UserClient: any process can read the IORegistry.
//
//  macOS System Information lists the name, VRAM and VBIOS of the GPU from
//  the same properties, but it cannot list the monitors under it: it shows
//  a PCI GPU's displays only for IOFramebuffer kernel objects below it, and
//  every CGVirtualDisplay under the Apple GPU. This view shows them.
//
//  No IOKit here (the reader is in MacLinuxGPUHostApp.swift), so the model
//  is tested offline (linuxu/tests/test_device_info.swift).
//

import Foundation

struct GPUConnectorInfo: Equatable {
    var name: String
    var status: String               // "connected", "disconnected", "unknown"
    var monitor: String?             // the EDID monitor name
    var widthMM: Int?
    var heightMM: Int?
    var preferred: (width: Int, height: Int, refresh: Int)?
    var lit: Bool
    var litMode: (width: Int, height: Int, refresh: Int)?

    static func == (a: GPUConnectorInfo, b: GPUConnectorInfo) -> Bool {
        a.name == b.name && a.status == b.status && a.monitor == b.monitor &&
            a.widthMM == b.widthMM && a.heightMM == b.heightMM &&
            a.preferred?.width == b.preferred?.width && a.preferred?.height == b.preferred?.height &&
            a.preferred?.refresh == b.preferred?.refresh && a.lit == b.lit &&
            a.litMode?.width == b.litMode?.width && a.litMode?.height == b.litMode?.height &&
            a.litMode?.refresh == b.litMode?.refresh
    }

    var connected: Bool { status == "connected" }
}

struct GPUDeviceInfo {
    var name: String?
    var nameSource: String?
    var vendorID: Int?
    var deviceID: Int?
    var revisionID: Int?
    var subsystemVendorID: Int?
    var subsystemID: Int?
    var pcieGeneration: Int?
    var pcieWidth: Int?
    var pcieSpeed: String?
    var driverProbed = false
    var gcVersion: String?
    var gfxTarget: String?
    var vramBytes: UInt64?
    var visibleVRAMBytes: UInt64?
    var vramType: String?
    var vramBitWidth: Int?
    var computeUnits: Int?
    var vbiosPartNumber: String?
    var vbiosVersion: String?
    var vbiosBuild: String?
    var vbiosDate: String?
    var fruProductName: String?
    /// The DRM connectors while the upstream driver runs with Display Core;
    /// nil otherwise.
    var hotplugEpoch: Int?
    var connectors: [GPUConnectorInfo]?

    /// From the dext service's registry properties; nil when the service
    /// publishes no identity (a driver from before this property set).
    init?(properties: [String: Any]) {
        guard let device = properties["MacLinuxGPUDevice"] as? [String: Any] else { return nil }
        func int(_ d: [String: Any], _ key: String) -> Int? { (d[key] as? NSNumber)?.intValue }
        func text(_ d: [String: Any], _ key: String) -> String? {
            guard let s = d[key] as? String, !s.isEmpty else { return nil }
            return s
        }
        name = text(device, "Name")
        nameSource = text(device, "NameSource")
        vendorID = int(device, "VendorID")
        deviceID = int(device, "DeviceID")
        revisionID = int(device, "RevisionID")
        subsystemVendorID = int(device, "SubsystemVendorID")
        subsystemID = int(device, "SubsystemID")
        pcieGeneration = int(device, "PCIeLinkGeneration")
        pcieWidth = int(device, "PCIeLinkWidth")
        pcieSpeed = text(device, "PCIeLinkSpeed")
        driverProbed = (device["DriverProbed"] as? Bool) ?? false
        gcVersion = text(device, "GCVersion")
        gfxTarget = text(device, "GFXTarget")
        vramBytes = (device["VRAMBytes"] as? NSNumber)?.uint64Value
        visibleVRAMBytes = (device["VisibleVRAMBytes"] as? NSNumber)?.uint64Value
        vramType = text(device, "VRAMType")
        vramBitWidth = int(device, "VRAMBitWidth")
        computeUnits = int(device, "ComputeUnits")
        vbiosPartNumber = text(device, "VBIOSPartNumber")
        vbiosVersion = text(device, "VBIOSVersion")
        vbiosBuild = text(device, "VBIOSBuild")
        vbiosDate = text(device, "VBIOSDate")
        fruProductName = text(device, "FRUProductName")
        if let displays = properties["MacLinuxGPUDisplays"] as? [String: Any] {
            hotplugEpoch = int(displays, "HotplugEpoch")
            let list = displays["Connectors"] as? [[String: Any]] ?? []
            connectors = list.compactMap { c in
                guard let name = text(c, "Name") else { return nil }
                var connector = GPUConnectorInfo(name: name, status: text(c, "Status") ?? "unknown",
                                                 monitor: text(c, "Monitor"),
                                                 widthMM: int(c, "WidthMM"), heightMM: int(c, "HeightMM"),
                                                 preferred: nil, lit: (c["Lit"] as? Bool) ?? false,
                                                 litMode: nil)
                if let w = int(c, "PreferredWidth"), let h = int(c, "PreferredHeight") {
                    connector.preferred = (w, h, int(c, "PreferredRefresh") ?? 0)
                }
                if connector.lit, let w = int(c, "LitWidth"), let h = int(c, "LitHeight") {
                    connector.litMode = (w, h, int(c, "LitRefresh") ?? 0)
                }
                return connector
            }
        }
    }

    /// "32 GB" for whole GiB, else MB, as System Information rounds VRAM.
    static func memoryText(_ bytes: UInt64) -> String {
        let gib: UInt64 = 1 << 30
        if bytes >= gib && bytes % gib == 0 { return "\(bytes / gib) GB" }
        return "\(bytes >> 20) MB"
    }

    static func modeText(_ mode: (width: Int, height: Int, refresh: Int)) -> String {
        mode.refresh > 0 ? "\(mode.width) x \(mode.height) @ \(mode.refresh) Hz"
                         : "\(mode.width) x \(mode.height)"
    }

    var title: String {
        if let name { return name }
        if let vendorID, let deviceID {
            return String(format: "AMD GPU %04x:%04x", vendorID, deviceID)
        }
        return "AMD GPU"
    }

    /// The GPU with the monitors on its outputs, laid out the way System
    /// Information lays out a graphics card.
    var summaryLines: [String] {
        var lines = ["\(title):"]
        func add(_ label: String, _ value: String?) {
            if let value, !value.isEmpty { lines.append("  \(label): \(value)") }
        }
        add("Chipset Model", name)
        add("Type", "External GPU")
        if let width = pcieWidth {
            add("PCIe Link", pcieSpeed.map { "\($0) x\(width)" } ?? "x\(width)")
        }
        if let vramBytes {
            var vram = GPUDeviceInfo.memoryText(vramBytes)
            if let vramType { vram += " \(vramType)" }
            if let vramBitWidth { vram += " (\(vramBitWidth)-bit)" }
            add("VRAM (Total)", vram)
        }
        if let computeUnits { add("Compute Units", "\(computeUnits)") }
        add("ISA Target", gfxTarget)
        add("GC Version", gcVersion)
        add("VBIOS Version", vbiosPartNumber)
        add("VBIOS Build", [vbiosVersion, vbiosBuild, vbiosDate].compactMap { $0 }.joined(separator: ", "))
        if let vendorID, let deviceID {
            var ids = String(format: "%04x:%04x", vendorID, deviceID)
            if let revisionID { ids += String(format: " rev %02x", revisionID) }
            if let subsystemVendorID, let subsystemID {
                ids += String(format: ", subsystem %04x:%04x", subsystemVendorID, subsystemID)
            }
            add("PCI ID", ids)
        }
        if !driverProbed {
            lines.append("  Driver: attached; the upstream driver has not probed the GPU yet")
        }
        if let connectors {
            lines.append("  Displays:")
            let shown = connectors.filter { $0.connected }
            if shown.isEmpty { lines.append("    (no monitor connected)") }
            for c in shown {
                lines.append("    \(c.monitor ?? "Monitor") on \(c.name):")
                if let mode = c.litMode {
                    lines.append("      Resolution: \(GPUDeviceInfo.modeText(mode)) (driven by this GPU)")
                } else if let preferred = c.preferred {
                    lines.append("      Preferred: \(GPUDeviceInfo.modeText(preferred)) (not driven)")
                }
                if let w = c.widthMM, let h = c.heightMM {
                    lines.append("      Size: \(w) x \(h) mm")
                }
            }
            let idle = connectors.filter { !$0.connected }.map(\.name)
            if !idle.isEmpty { lines.append("    Unused outputs: \(idle.joined(separator: ", "))") }
        } else if driverProbed {
            lines.append("  Displays: not reported (Display Core off, or no display client yet)")
        }
        return lines
    }
}
