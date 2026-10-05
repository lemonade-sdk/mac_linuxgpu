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

/// The PCI Express capability of a device, from its configuration space
/// (the sysfs "config" file): what it can do and what it is set to. Linux
/// sets Max Payload and Max Read Request (pcie_bus_configure_settings,
/// pcie_set_readrq), and Extended and 10-bit tags (pci_configure_extended_tags,
/// pci_configure_10bit_tags); a device left at its reset values moves data
/// in smaller requests than it could.
struct PCIeCapability: Equatable {
    let offset: Int
    let devCap, devCap2: UInt32
    let devCtl, devSta, lnkSta, devCtl2: UInt16
    let lnkCap: UInt32

    /// Walks the capability list from 0x34 for capability 0x10; nil when the
    /// space is too short, the list loops or there is no such capability.
    init?(config: Data) {
        let b = [UInt8](config)
        func u16(_ at: Int) -> UInt16? { at + 2 <= b.count ? UInt16(b[at]) | UInt16(b[at + 1]) << 8 : nil }
        func u32(_ at: Int) -> UInt32? {
            guard let lo = u16(at), let hi = u16(at + 2) else { return nil }
            return UInt32(lo) | UInt32(hi) << 16
        }
        guard b.count >= 64, b[6] & 0x10 != 0 else { return nil }   // Status: Capabilities List
        var at = Int(b[0x34] & 0xfc), seen = 0
        while at >= 0x40 && at + 2 <= b.count && seen < 48 {
            if b[at] == 0x10 { break }
            at = Int(b[at + 1] & 0xfc)
            seen += 1
        }
        guard at >= 0x40, at + 2 <= b.count, b[at] == 0x10,
              let devCap = u32(at + 4), let devCtl = u16(at + 8), let devSta = u16(at + 10),
              let lnkCap = u32(at + 12), let lnkSta = u16(at + 18),
              let devCap2 = u32(at + 36), let devCtl2 = u16(at + 40) else { return nil }
        offset = at
        self.devCap = devCap; self.devCtl = devCtl; self.devSta = devSta
        self.lnkCap = lnkCap; self.lnkSta = lnkSta; self.devCap2 = devCap2; self.devCtl2 = devCtl2
    }

    var maxPayloadSupported: Int { 128 << Int(devCap & 7) }
    var maxPayload: Int { 128 << Int((devCtl >> 5) & 7) }
    var maxReadRequest: Int { 128 << Int((devCtl >> 12) & 7) }
    var extendedTagSupported: Bool { devCap & (1 << 5) != 0 }
    var extendedTagEnabled: Bool { devCtl & (1 << 8) != 0 }
    var relaxedOrdering: Bool { devCtl & (1 << 4) != 0 }
    var noSnoop: Bool { devCtl & (1 << 11) != 0 }
    var tag10Completer: Bool { devCap2 & (1 << 16) != 0 }
    var tag10Requester: Bool { devCap2 & (1 << 17) != 0 }
    var tag10Enabled: Bool { devCtl2 & (1 << 12) != 0 }
    var linkSpeed: Int { Int(lnkSta & 0xf) }
    var linkWidth: Int { Int((lnkSta >> 4) & 0x3f) }
    var maxLinkSpeed: Int { Int(lnkCap & 0xf) }
    var maxLinkWidth: Int { Int((lnkCap >> 4) & 0x3f) }

    var lines: [String] {
        let speeds = ["?", "2.5", "5.0", "8.0", "16.0", "32.0", "64.0"]
        func gt(_ s: Int) -> String { s < speeds.count ? speeds[s] + " GT/s" : "?" }
        return [
            String(format: "PCI Express capability at 0x%02x: DevCap 0x%08x DevCtl 0x%04x DevSta 0x%04x DevCap2 0x%08x DevCtl2 0x%04x",
                   offset, devCap, devCtl, devSta, devCap2, devCtl2),
            "  link: \(gt(linkSpeed)) x\(linkWidth) (the device can do \(gt(maxLinkSpeed)) x\(maxLinkWidth))",
            "  max payload: \(maxPayload) bytes (supports \(maxPayloadSupported))",
            "  max read request: \(maxReadRequest) bytes",
            "  extended tags (8-bit): \(extendedTagEnabled ? "on" : "off") (\(extendedTagSupported ? "supported" : "not supported"))",
            "  10-bit tags as requester: \(tag10Enabled ? "on" : "off") (requester \(tag10Requester ? "supported" : "not supported"), completer \(tag10Completer ? "supported" : "not supported"))",
            "  relaxed ordering \(relaxedOrdering ? "on" : "off"), no snoop \(noSnoop ? "on" : "off")",
        ]
    }
}

/// The Resizable BAR extended capability (ID 0x15, version 1) of a device,
/// from its configuration space (the sysfs "config" file): for each
/// resizable BAR, the sizes it can decode and the size it decodes now.
/// Validated as linuxu's pci_rebar_read validates it (linuxu/src/pci/
/// pci_stub.c): one to six entries inside the space, each naming a distinct
/// BAR whose selected size is one it supports. Sizes above 128 TB (the
/// Control register's upper bits) are not read.
struct ResizableBARCapability: Equatable {
    struct Entry: Equatable {
        let bar: Int
        let supported: UInt32      // bit n: 1 MB << n
        let selected: Int          // the size now decoded, 1 MB << selected

        var supportedBytes: [UInt64] { (0..<28).filter { supported & (1 << $0) != 0 }.map { UInt64(1) << ($0 + 20) } }
        var currentBytes: UInt64 { UInt64(1) << (selected + 20) }
        var largestBytes: UInt64 { supportedBytes.last ?? 0 }
    }

    let offset: Int
    let entries: [Entry]

    /// Walks the extended capability list from 0x100 for capability 0x15;
    /// nil when the space is too short, the list loops or leaves the space,
    /// there is no such capability, or the capability is malformed.
    init?(config: Data) {
        let b = [UInt8](config)
        func u32(_ at: Int) -> UInt32? {
            guard at >= 0, at + 4 <= b.count else { return nil }
            return UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
        }
        var at = 0x100, seen = 0
        var header: UInt32 = 0
        while true {
            guard seen < (4096 - 0x100) / 4, let h = u32(at), h != 0, h != UInt32.max else { return nil }
            if h & 0xffff == 0x15 { header = h; break }
            let next = Int(h >> 20)
            guard next >= 0x100, next & 3 == 0, next != at else { return nil }
            at = next
            seen += 1
        }
        guard (header >> 16) & 0xf == 1, let first = u32(at + 8) else { return nil }
        let count = Int((first >> 5) & 7)
        guard count >= 1, count <= 6, at + 4 + count * 8 <= b.count else { return nil }
        var entries: [Entry] = []
        var bars: UInt32 = 0
        for i in 0..<count {
            guard let capability = u32(at + 4 + i * 8), let control = u32(at + 8 + i * 8),
                  capability != UInt32.max, control != UInt32.max else { return nil }
            let bar = Int(control & 7), selected = Int((control >> 8) & 0x3f)
            let supported = capability >> 4
            guard bar < 6, bars & (1 << bar) == 0, selected < 28,
                  supported & (1 << selected) != 0 else { return nil }
            bars |= 1 << bar
            entries.append(Entry(bar: bar, supported: supported, selected: selected))
        }
        offset = at
        self.entries = entries
    }

    func entry(bar: Int) -> Entry? { entries.first { $0.bar == bar } }

    static func sizeText(_ bytes: UInt64) -> String {
        bytes >= UInt64(1) << 30 ? "\(bytes >> 30) GB" : "\(bytes >> 20) MB"
    }

    var lines: [String] {
        [String(format: "Resizable BAR capability at 0x%03x:", offset)] + entries.map { e in
            "  BAR\(e.bar): \(Self.sizeText(e.currentBytes)) now (supports "
                + e.supportedBytes.map(Self.sizeText).joined(separator: ", ") + ")"
        }
    }
}
