/* The properties the dext sets on its IOService (device_identity.h says
 * which keys System Information reads and why they reach it), as DriverKit
 * containers for IOService::SetProperties().
 *
 * Include after the DriverKit container headers (OSDictionary, OSArray,
 * OSString, OSNumber, OSBoolean); the offline test includes its own
 * substitutes instead (linuxu/tests/test_device_properties.cpp). */
#ifndef MACLINUXGPU_DEVICE_PROPERTIES_H
#define MACLINUXGPU_DEVICE_PROPERTIES_H

#include "device_identity.h"

namespace maclinuxgpu {
namespace properties {

/* Each helper consumes @object (one reference) and fails on NULL. */
inline bool put(OSDictionary *dict, const char *key, OSObject *object)
{
    if (!object) return false;
    const bool ok = dict->setObject(key, object);
    object->release();
    return ok;
}

/* An empty or absent string is not published. */
inline bool put_string(OSDictionary *dict, const char *key, const char *value)
{
    if (!value || !value[0]) return true;
    return put(dict, key, OSString::withCString(value));
}

inline bool put_number(OSDictionary *dict, const char *key, uint64_t value, size_t bits)
{
    return put(dict, key, OSNumber::withNumber(value, bits));
}

inline bool put_bool(OSDictionary *dict, const char *key, bool value)
{
    return dict->setObject(key, value ? kOSBooleanTrue : kOSBooleanFalse);
}

inline OSDictionary *device_dictionary(const DeviceIdentity &id)
{
    OSDictionary *dict = OSDictionary::withCapacity(24);
    if (!dict) return nullptr;
    const char *source = nullptr;
    const char *name = product_name(id, &source);
    bool ok = put_string(dict, kMLGDeviceName, name) &&
              put_string(dict, kMLGDeviceNameSource, source) &&
              put_bool(dict, kMLGDeviceDriverProbed, id.probed);
    if (ok && id.pci) {
        ok = put_number(dict, kMLGDeviceVendorID, id.vendor, 32) &&
             put_number(dict, kMLGDeviceDeviceID, id.device, 32) &&
             put_number(dict, kMLGDeviceRevisionID, id.revision, 32) &&
             put_number(dict, kMLGDeviceSubsystemVendorID, id.subsystemVendor, 32) &&
             put_number(dict, kMLGDeviceSubsystemID, id.subsystem, 32);
    }
    if (ok && id.link) {
        const PCIeLink link = pcie_link(id.linkStatus);
        ok = put_number(dict, kMLGDevicePCIeGeneration, link.generation, 32) &&
             put_number(dict, kMLGDevicePCIeWidth, link.width, 32) &&
             put_string(dict, kMLGDevicePCIeSpeed, pcie_speed(link.generation));
    }
    if (ok && id.probed) {
        const struct rt_device_identity &d = id.driver;
        char gc[12];
        ok = put_string(dict, kMLGDeviceGCVersion, ip_version_text(d.gc_version, gc)) &&
             put_string(dict, kMLGDeviceGFXTarget, d.gfx_target) &&
             (!d.vram_bytes || put_number(dict, kMLGDeviceVRAMBytes, d.vram_bytes, 64)) &&
             (!d.visible_vram_bytes ||
              put_number(dict, kMLGDeviceVisibleVRAMBytes, d.visible_vram_bytes, 64)) &&
             (!d.vram_type || put_string(dict, kMLGDeviceVRAMType, d.vram_type_name)) &&
             (!d.vram_bit_width || put_number(dict, kMLGDeviceVRAMBitWidth, d.vram_bit_width, 32)) &&
             (!d.compute_units || put_number(dict, kMLGDeviceComputeUnits, d.compute_units, 32)) &&
             put_string(dict, kMLGDeviceVBIOSPartNumber, d.vbios_pn) &&
             put_string(dict, kMLGDeviceVBIOSVersion, d.vbios_version) &&
             put_string(dict, kMLGDeviceVBIOSBuild, d.vbios_build) &&
             put_string(dict, kMLGDeviceVBIOSDate, d.vbios_date) &&
             put_string(dict, kMLGDeviceFRUProductName, d.product_name);
    }
    if (!ok) {
        dict->release();
        return nullptr;
    }
    return dict;
}

} // namespace properties

/* The properties of the GPU's identity: System Information's keys and
 * MacLinuxGPUDevice. A new dictionary the caller releases, NULL when an
 * allocation failed. */
inline OSDictionary *identity_properties(const DeviceIdentity &id)
{
    using namespace properties;
    OSDictionary *dict = OSDictionary::withCapacity(8);
    if (!dict) return nullptr;
    const uint32_t vramMB = vram_total_mb(id);
    bool ok = put_string(dict, kSPModel, product_name(id, nullptr)) &&
              (!vramMB || put_number(dict, kSPVRAMTotalMB, vramMB, 32)) &&
              (!id.probed || (put_string(dict, kSPVBIOSVersion, id.driver.vbios_pn) &&
                              put_string(dict, kSPROMRevision, id.driver.vbios_version))) &&
              put(dict, kMLGDevice, device_dictionary(id));
    if (!ok) {
        dict->release();
        return nullptr;
    }
    return dict;
}

/* MacLinuxGPUDisplays: { kMLGDisplays: { HotplugEpoch, Connectors } }. A
 * new dictionary the caller releases, NULL when an allocation failed. */
inline OSDictionary *display_properties(const DisplayState &state)
{
    using namespace properties;
    OSDictionary *outer = OSDictionary::withCapacity(1);
    OSDictionary *displays = OSDictionary::withCapacity(2);
    OSArray *connectors = OSArray::withCapacity(state.count ? state.count : 1);
    bool ok = outer && displays && connectors &&
              put_number(displays, kMLGDisplaysEpoch, state.epoch, 32);
    for (uint32_t i = 0; ok && i < state.count && i < RT_DISPLAY_CONNECTORS_MAX; ++i) {
        const ConnectorState &c = state.connector[i];
        OSDictionary *entry = OSDictionary::withCapacity(12);
        ok = entry && put_string(entry, kMLGConnectorName, c.name) &&
             put_string(entry, kMLGConnectorStatus, connector_status(c.status)) &&
             put_string(entry, kMLGConnectorMonitor, c.monitor) &&
             put_bool(entry, kMLGConnectorLit, c.lit != 0);
        if (ok && c.widthMM && c.heightMM)
            ok = put_number(entry, kMLGConnectorWidthMM, c.widthMM, 32) &&
                 put_number(entry, kMLGConnectorHeightMM, c.heightMM, 32);
        if (ok && c.preferredWidth && c.preferredHeight)
            ok = put_number(entry, kMLGConnectorPreferredWidth, c.preferredWidth, 32) &&
                 put_number(entry, kMLGConnectorPreferredHeight, c.preferredHeight, 32) &&
                 put_number(entry, kMLGConnectorPreferredRefresh, c.preferredRefresh, 32);
        if (ok && c.lit)
            ok = put_number(entry, kMLGConnectorLitWidth, c.litWidth, 32) &&
                 put_number(entry, kMLGConnectorLitHeight, c.litHeight, 32) &&
                 put_number(entry, kMLGConnectorLitRefresh, c.litRefresh, 32);
        if (ok) ok = connectors->setObject(entry);
        if (entry) entry->release();
    }
    if (ok) {
        ok = displays->setObject(kMLGDisplaysConnectors, connectors) &&
             outer->setObject(kMLGDisplays, displays);
    }
    if (connectors) connectors->release();
    if (displays) displays->release();
    if (!ok) {
        if (outer) outer->release();
        return nullptr;
    }
    return outer;
}

} // namespace maclinuxgpu

#endif
