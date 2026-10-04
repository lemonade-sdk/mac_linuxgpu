/* The GPU's identity as the dext publishes it on its IOService, for macOS
 * System Information and for our own tools.
 *
 * System Information (SPDisplaysReporter and SPPCIReporter in
 * /System/Library/SystemProfiler) builds each PCI GPU's entry from the
 * IOPCIDevice's properties merged with the properties of each of its
 * direct children in the IOService plane (IORegistryEntryGetChildIterator
 * and addEntriesFromDictionary:, children's keys win). The dext's service
 * is such a child, and properties a dext sets with SetProperties() reach
 * user space as top-level keys (the kernel merges IOUserServiceProperties
 * into the dictionary IORegistryEntryCreateCFProperties returns). The keys
 * it reads for a PCI GPU, and how it shows them:
 *
 *   model              "Chipset Model" and the GPU's name (SPDisplays), the
 *                      device's name (SPPCI); string or data
 *   VRAM,totalMB       "VRAM (Total)", in MB, shown as GB from 1024 MB
 *   ATY,EFIVersionB    "VBIOS Version"
 *   rom-revision       "ROM Revision" (SPDisplays and SPPCI)
 *
 * Nothing else a dext can publish places an entry there: "Displays:" under
 * a PCI GPU lists only the framebuffers below it that are IOFramebuffer
 * kernel objects (IOGraphicsFamily, which has no DriverKit family), and
 * every CGVirtualDisplay is listed under the Apple GPU. The monitors on
 * the GPU's outputs are published in MacLinuxGPUDisplays for our tools.
 *
 * Our own keys:
 *   MacLinuxGPUDevice    dictionary, kMLGDevice* below
 *   MacLinuxGPUDisplays  dictionary: HotplugEpoch, Connectors (array of
 *                        dictionaries, kMLGConnector* below); present while
 *                        the upstream driver runs with Display Core
 *
 * The values come from the provider's PCI registers as IOPCIFamily
 * publishes them, the product-name table of libdrm (rt/amdgpu_ids.h) and,
 * after the upstream probe, the amdgpu device (rt/identity.h) and its DRM
 * connectors (rt/display.h). Nothing is assumed per board.
 *
 * device_properties.h turns these into DriverKit containers. */
#ifndef MACLINUXGPU_DEVICE_IDENTITY_H
#define MACLINUXGPU_DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rt/amdgpu_ids.h>
#include <rt/display.h>
#include <rt/identity.h>

namespace maclinuxgpu {

/* System Information's keys. */
constexpr const char *kSPModel = "model";
constexpr const char *kSPVRAMTotalMB = "VRAM,totalMB";
constexpr const char *kSPVBIOSVersion = "ATY,EFIVersionB";
constexpr const char *kSPROMRevision = "rom-revision";

/* Our keys. */
constexpr const char *kMLGDevice = "MacLinuxGPUDevice";
constexpr const char *kMLGDeviceName = "Name";
constexpr const char *kMLGDeviceNameSource = "NameSource";	/* "amdgpu.ids", "FRU" */
constexpr const char *kMLGDeviceVendorID = "VendorID";
constexpr const char *kMLGDeviceDeviceID = "DeviceID";
constexpr const char *kMLGDeviceRevisionID = "RevisionID";
constexpr const char *kMLGDeviceSubsystemVendorID = "SubsystemVendorID";
constexpr const char *kMLGDeviceSubsystemID = "SubsystemID";
constexpr const char *kMLGDevicePCIeGeneration = "PCIeLinkGeneration";
constexpr const char *kMLGDevicePCIeSpeed = "PCIeLinkSpeed";
constexpr const char *kMLGDevicePCIeWidth = "PCIeLinkWidth";
constexpr const char *kMLGDeviceDriverProbed = "DriverProbed";
constexpr const char *kMLGDeviceGCVersion = "GCVersion";
constexpr const char *kMLGDeviceGFXTarget = "GFXTarget";
constexpr const char *kMLGDeviceVRAMBytes = "VRAMBytes";
constexpr const char *kMLGDeviceVisibleVRAMBytes = "VisibleVRAMBytes";
constexpr const char *kMLGDeviceVRAMType = "VRAMType";
constexpr const char *kMLGDeviceVRAMBitWidth = "VRAMBitWidth";
constexpr const char *kMLGDeviceComputeUnits = "ComputeUnits";
constexpr const char *kMLGDeviceVBIOSPartNumber = "VBIOSPartNumber";
constexpr const char *kMLGDeviceVBIOSVersion = "VBIOSVersion";
constexpr const char *kMLGDeviceVBIOSBuild = "VBIOSBuild";
constexpr const char *kMLGDeviceVBIOSDate = "VBIOSDate";
constexpr const char *kMLGDeviceFRUProductName = "FRUProductName";

constexpr const char *kMLGDisplays = "MacLinuxGPUDisplays";
constexpr const char *kMLGDisplaysEpoch = "HotplugEpoch";
constexpr const char *kMLGDisplaysConnectors = "Connectors";
constexpr const char *kMLGConnectorName = "Name";
constexpr const char *kMLGConnectorStatus = "Status";	/* "connected", "disconnected", "unknown" */
constexpr const char *kMLGConnectorMonitor = "Monitor";
constexpr const char *kMLGConnectorWidthMM = "WidthMM";
constexpr const char *kMLGConnectorHeightMM = "HeightMM";
constexpr const char *kMLGConnectorPreferredWidth = "PreferredWidth";
constexpr const char *kMLGConnectorPreferredHeight = "PreferredHeight";
constexpr const char *kMLGConnectorPreferredRefresh = "PreferredRefresh";
constexpr const char *kMLGConnectorLit = "Lit";
constexpr const char *kMLGConnectorLitWidth = "LitWidth";
constexpr const char *kMLGConnectorLitHeight = "LitHeight";
constexpr const char *kMLGConnectorLitRefresh = "LitRefresh";

/* The PCI Express Link Status register as IOPCIFamily publishes it
 * (IOPCIExpressLinkStatus): current link speed in bits 3:0 (the index of
 * the generation, 1 = 2.5 GT/s), negotiated width in bits 9:4. */
struct PCIeLink {
    uint32_t generation;
    uint32_t width;
};

inline PCIeLink pcie_link(uint64_t status)
{
    return PCIeLink{ (uint32_t)(status & 0xf), (uint32_t)((status >> 4) & 0x3f) };
}

/* Transfer rate of a link generation; NULL for one the PCIe base
 * specification does not define. */
inline const char *pcie_speed(uint32_t generation)
{
    static const char *const speeds[] = { nullptr, "2.5 GT/s", "5.0 GT/s", "8.0 GT/s",
                                          "16.0 GT/s", "32.0 GT/s", "64.0 GT/s" };
    return generation < sizeof(speeds) / sizeof(speeds[0]) ? speeds[generation] : nullptr;
}

/* IOPCIFamily publishes the configuration registers it names (vendor-id,
 * device-id, revision-id, subsystem-vendor-id, subsystem-id) as 4-byte
 * little-endian data. */
inline bool pci_register_value(const void *bytes, size_t length, uint32_t &out)
{
    if (!bytes || length != 4) return false;
    const uint8_t *b = static_cast<const uint8_t *>(bytes);
    out = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

/* What the dext knows about its GPU at a point in time. */
struct DeviceIdentity {
    /* The provider's registers (IOPCIFamily's vendor-id, device-id, ...). */
    bool pci;
    uint16_t vendor, device, subsystemVendor, subsystem;
    uint8_t revision;
    bool link;
    uint64_t linkStatus;
    /* The upstream driver's view, once its probe succeeded. */
    bool probed;
    struct rt_device_identity driver;
};

/* The board's product name: libdrm's table, else the FRU board name the
 * driver read; NULL without either (no name is made up: System
 * Information then keeps its generic "Display"). @source says which
 * ("amdgpu.ids", "FRU"). */
inline const char *product_name(const DeviceIdentity &id, const char **source)
{
    if (source) *source = nullptr;
    if (id.pci && id.vendor == 0x1002) {
        const char *name = amdgpu_ids_name(id.device, id.revision);
        if (name) {
            if (source) *source = "amdgpu.ids";
            return name;
        }
    }
    if (id.probed && id.driver.product_name[0]) {
        if (source) *source = "FRU";
        return id.driver.product_name;
    }
    return nullptr;
}

/* Decimal text of @value into @out (at least 11 bytes); returns @out. */
inline char *decimal(uint32_t value, char *out)
{
    char digits[10];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (size_t i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    out[n] = '\0';
    return out;
}

/* "major.minor.revision" of an IP_VERSION() value as amdgpu encodes it
 * (major bits 31:24, minor 23:16, revision 15:8, variant and subrevision
 * below), at least 12 bytes; empty for 0. */
inline char *ip_version_text(uint32_t version, char *out)
{
    out[0] = '\0';
    if (!version) return out;
    const uint32_t parts[3] = { version >> 24, (version >> 16) & 0xff, (version >> 8) & 0xff };
    size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        if (i) out[at++] = '.';
        decimal(parts[i], out + at);
        while (out[at]) ++at;
    }
    return out;
}

/* VRAM in whole MB for VRAM,totalMB; 0 when unknown or out of range. */
inline uint32_t vram_total_mb(const DeviceIdentity &id)
{
    if (!id.probed) return 0;
    const uint64_t mb = id.driver.vram_bytes >> 20;
    return mb <= UINT32_MAX ? (uint32_t)mb : 0;
}

/* One DRM connector as published. */
struct ConnectorState {
    char name[RT_DISPLAY_NAME_BYTES];
    uint32_t status;			/* enum drm_connector_status */
    char monitor[RT_DISPLAY_MONITOR_NAME_BYTES];
    uint32_t widthMM, heightMM;
    uint32_t preferredWidth, preferredHeight, preferredRefresh;
    uint32_t lit, litWidth, litHeight, litRefresh;
};

struct DisplayState {
    uint32_t epoch;
    uint32_t count;
    ConnectorState connector[RT_DISPLAY_CONNECTORS_MAX];
};

inline const char *connector_status(uint32_t status)
{
    switch (status) {
    case 1: return "connected";
    case 2: return "disconnected";
    default: return "unknown";
    }
}

/* The connectors of a report; the monitors come from rt_display_monitor(),
 * called with each connector's name by @monitor (may be NULL). */
template <typename MonitorLookup>
inline void display_state(const struct rt_display_report &report, MonitorLookup monitor,
                          DisplayState &out)
{
    memset(&out, 0, sizeof(out));
    out.epoch = report.hotplug_epoch;
    const uint32_t n = report.connectors < RT_DISPLAY_CONNECTORS_MAX
        ? report.connectors : RT_DISPLAY_CONNECTORS_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        const struct rt_display_connector &in = report.connector[i];
        ConnectorState &c = out.connector[out.count++];
        memcpy(c.name, in.name, sizeof(c.name));
        c.name[sizeof(c.name) - 1] = '\0';
        c.status = in.status;
        c.preferredWidth = in.preferred_width;
        c.preferredHeight = in.preferred_height;
        c.preferredRefresh = in.preferred_refresh;
        c.lit = in.lit;
        c.litWidth = in.lit_width;
        c.litHeight = in.lit_height;
        c.litRefresh = in.lit_refresh;
        struct rt_display_monitor m;
        memset(&m, 0, sizeof(m));
        if (monitor(c.name, m) == 0) {
            memcpy(c.monitor, m.name, sizeof(c.monitor));
            c.monitor[sizeof(c.monitor) - 1] = '\0';
            c.widthMM = m.width_mm;
            c.heightMM = m.height_mm;
        }
    }
}

/* Whether two states publish the same properties (the hotplug epoch
 * included: a changed epoch is news to a reader). */
inline bool display_state_equal(const DisplayState &a, const DisplayState &b)
{
    return memcmp(&a, &b, sizeof(a)) == 0;
}

} // namespace maclinuxgpu

#endif
