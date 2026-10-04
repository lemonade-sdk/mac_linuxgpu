/* The properties the dext publishes on its IOService for System
 * Information and our tools (dext/sources/device_identity.h,
 * device_properties.h), built with DriverKit container substitutes
 * (driverkit_property_mocks.h): the keys and value types System
 * Information reads, what is left out when unknown, the connector list,
 * and that every allocation failure returns NULL without leaking. */
#include "driverkit_property_mocks.h"
#include "../../dext/sources/device_properties.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace maclinuxgpu;

static const OSString *string_at(const OSDictionary *d, const char *key)
{
    return dynamic_cast<const OSString *>(d->getObject(key));
}

static const OSNumber *number_at(const OSDictionary *d, const char *key)
{
    return dynamic_cast<const OSNumber *>(d->getObject(key));
}

static bool string_is(const OSDictionary *d, const char *key, const char *value)
{
    const OSString *s = string_at(d, key);
    return s && s->value == value;
}

static bool number_is(const OSDictionary *d, const char *key, uint64_t value, size_t bits)
{
    const OSNumber *n = number_at(d, key);
    return n && n->value == value && n->bits == bits;
}

/* The R9700 of the first hardware runs: AMD 0x7551 rev 0xc0 behind a
 * Thunderbolt enclosure, subsystem 1da2:e499, x16 at 32 GT/s
 * (IOPCIExpressLinkStatus 4357 = 0x1105). */
static DeviceIdentity r9700_at_start()
{
    DeviceIdentity id;
    memset(&id, 0, sizeof(id));
    id.pci = true;
    id.vendor = 0x1002;
    id.device = 0x7551;
    id.revision = 0xc0;
    id.subsystemVendor = 0x1da2;
    id.subsystem = 0xe499;
    id.link = true;
    id.linkStatus = 4357;
    return id;
}

static DeviceIdentity r9700_probed()
{
    DeviceIdentity id = r9700_at_start();
    id.probed = true;
    struct rt_device_identity &d = id.driver;
    d.version = RT_IDENTITY_VERSION;
    d.vendor = 0x1002;
    d.device = 0x7551;
    d.revision = 0xc0;
    d.gc_version = (12u << 24) | (0u << 16) | (1u << 8);	/* IP_VERSION(12, 0, 1) */
    d.gfx_target_version = 120001;
    strcpy(d.gfx_target, "gfx1201");
    d.vram_bytes = 32ull << 30;
    d.visible_vram_bytes = 256ull << 20;
    d.vram_type = 9;
    strcpy(d.vram_type_name, "GDDR6");
    d.vram_bit_width = 256;
    d.compute_units = 64;
    strcpy(d.vbios_pn, "113-EXAMPLE-PN");
    strcpy(d.vbios_version, "022.001.002.008.000001");
    strcpy(d.vbios_build, "EXAMPLE BUILD");
    strcpy(d.vbios_date, "2026/01/01 00:00");
    return id;
}

static void test_helpers()
{
    /* "device-id" = <51750000>, "revision-id" = <c0000000> in ioreg. */
    uint32_t value = 0;
    const uint8_t device_id[4] = { 0x51, 0x75, 0x00, 0x00 };
    assert(pci_register_value(device_id, 4, value) && value == 0x7551);
    assert(!pci_register_value(device_id, 2, value) && !pci_register_value(nullptr, 4, value));
    const PCIeLink link = pcie_link(4357);
    assert(link.generation == 5 && link.width == 16);
    assert(!strcmp(pcie_speed(1), "2.5 GT/s") && !strcmp(pcie_speed(5), "32.0 GT/s"));
    assert(!pcie_speed(0) && !pcie_speed(7));
    char text[12];
    assert(!strcmp(decimal(0, text), "0") && !strcmp(decimal(4294967295u, text), "4294967295"));
    assert(!strcmp(ip_version_text((12u << 24) | (1u << 8), text), "12.0.1"));
    assert(!strcmp(ip_version_text((9u << 24) | (4u << 16) | (3u << 8), text), "9.4.3"));
    /* Variant and subrevision (bits 7:0) are not part of the name. */
    assert(!strcmp(ip_version_text((11u << 24) | (5u << 16) | (2u << 8) | 0x11, text), "11.5.2"));
    assert(!strcmp(ip_version_text(0, text), ""));
    assert(!strcmp(connector_status(1), "connected") && !strcmp(connector_status(2), "disconnected") &&
           !strcmp(connector_status(3), "unknown") && !strcmp(connector_status(0), "unknown"));

    DeviceIdentity id = r9700_at_start();
    const char *source = nullptr;
    assert(!strcmp(product_name(id, &source), "AMD Radeon AI Pro R9700") &&
           !strcmp(source, "amdgpu.ids"));
    id.revision = 0xc1;
    assert(!strcmp(product_name(id, nullptr), "AMD Radeon AI Pro R9700S"));
    /* A revision libdrm does not list: no name, not a guess. */
    id.revision = 0xc5;
    assert(!product_name(id, &source) && !source);
    /* The FRU board name when the table has none. */
    id.probed = true;
    strcpy(id.driver.product_name, "Example FRU Board");
    assert(!strcmp(product_name(id, &source), "Example FRU Board") && !strcmp(source, "FRU"));
    /* Another vendor's device is never looked up in AMD's table. */
    id = r9700_at_start();
    id.vendor = 0x10de;
    assert(!product_name(id, nullptr));

    assert(vram_total_mb(r9700_at_start()) == 0);
    assert(vram_total_mb(r9700_probed()) == 32768);
}

/* At Start: only the provider's registers are known. */
static void test_identity_at_start()
{
    OSDictionary *p = identity_properties(r9700_at_start());
    assert(p);
    assert(string_is(p, kSPModel, "AMD Radeon AI Pro R9700"));
    assert(!p->getObject(kSPVRAMTotalMB) && !p->getObject(kSPVBIOSVersion) &&
           !p->getObject(kSPROMRevision));
    auto *device = dynamic_cast<const OSDictionary *>(p->getObject(kMLGDevice));
    assert(device);
    assert(string_is(device, kMLGDeviceName, "AMD Radeon AI Pro R9700") &&
           string_is(device, kMLGDeviceNameSource, "amdgpu.ids"));
    assert(device->getObject(kMLGDeviceDriverProbed) == kOSBooleanFalse);
    assert(number_is(device, kMLGDeviceVendorID, 0x1002, 32) &&
           number_is(device, kMLGDeviceDeviceID, 0x7551, 32) &&
           number_is(device, kMLGDeviceRevisionID, 0xc0, 32) &&
           number_is(device, kMLGDeviceSubsystemVendorID, 0x1da2, 32) &&
           number_is(device, kMLGDeviceSubsystemID, 0xe499, 32));
    assert(number_is(device, kMLGDevicePCIeGeneration, 5, 32) &&
           number_is(device, kMLGDevicePCIeWidth, 16, 32) &&
           string_is(device, kMLGDevicePCIeSpeed, "32.0 GT/s"));
    assert(!device->getObject(kMLGDeviceVRAMBytes) && !device->getObject(kMLGDeviceGFXTarget) &&
           !device->getObject(kMLGDeviceVBIOSPartNumber));
    assert(p->getCount() == 2);
    p->release();
    assert(mock_live_objects == 0);
}

/* After the upstream probe: System Information's VRAM and VBIOS keys. */
static void test_identity_probed()
{
    OSDictionary *p = identity_properties(r9700_probed());
    assert(p);
    assert(string_is(p, kSPModel, "AMD Radeon AI Pro R9700"));
    /* System Information shows VRAM,totalMB >= 1024 as "%.2g GB": 32 GB. */
    assert(number_is(p, kSPVRAMTotalMB, 32768, 32));
    assert(string_is(p, kSPVBIOSVersion, "113-EXAMPLE-PN"));
    assert(string_is(p, kSPROMRevision, "022.001.002.008.000001"));
    auto *device = dynamic_cast<const OSDictionary *>(p->getObject(kMLGDevice));
    assert(device && device->getObject(kMLGDeviceDriverProbed) == kOSBooleanTrue);
    assert(string_is(device, kMLGDeviceGCVersion, "12.0.1") &&
           string_is(device, kMLGDeviceGFXTarget, "gfx1201"));
    assert(number_is(device, kMLGDeviceVRAMBytes, 32ull << 30, 64) &&
           number_is(device, kMLGDeviceVisibleVRAMBytes, 256ull << 20, 64) &&
           string_is(device, kMLGDeviceVRAMType, "GDDR6") &&
           number_is(device, kMLGDeviceVRAMBitWidth, 256, 32) &&
           number_is(device, kMLGDeviceComputeUnits, 64, 32));
    assert(string_is(device, kMLGDeviceVBIOSPartNumber, "113-EXAMPLE-PN") &&
           string_is(device, kMLGDeviceVBIOSVersion, "022.001.002.008.000001") &&
           string_is(device, kMLGDeviceVBIOSBuild, "EXAMPLE BUILD") &&
           string_is(device, kMLGDeviceVBIOSDate, "2026/01/01 00:00"));
    assert(!device->getObject(kMLGDeviceFRUProductName));
    p->release();
    assert(mock_live_objects == 0);

    /* What the driver does not know is left out, not published as zero. */
    DeviceIdentity id = r9700_probed();
    id.driver.vram_bytes = 0;
    id.driver.vram_type = 0;
    id.driver.vbios_pn[0] = id.driver.vbios_version[0] = '\0';
    id.driver.gfx_target[0] = '\0';
    id.driver.compute_units = 0;
    id.revision = 0xc5;
    p = identity_properties(id);
    assert(p && !p->getObject(kSPModel) && !p->getObject(kSPVRAMTotalMB) &&
           !p->getObject(kSPVBIOSVersion) && !p->getObject(kSPROMRevision));
    device = dynamic_cast<const OSDictionary *>(p->getObject(kMLGDevice));
    assert(device && !device->getObject(kMLGDeviceName) && !device->getObject(kMLGDeviceNameSource) &&
           !device->getObject(kMLGDeviceVRAMBytes) && !device->getObject(kMLGDeviceVRAMType) &&
           !device->getObject(kMLGDeviceGFXTarget) && !device->getObject(kMLGDeviceComputeUnits) &&
           string_is(device, kMLGDeviceGCVersion, "12.0.1"));
    p->release();
    assert(mock_live_objects == 0);
}

static struct rt_display_report two_connectors()
{
    struct rt_display_report r;
    memset(&r, 0, sizeof(r));
    r.version = RT_DISPLAY_VERSION;
    r.hotplug_epoch = 3;
    r.connectors = 2;
    strcpy(r.connector[0].name, "DP-4");
    r.connector[0].status = 1;
    r.connector[0].modes = 12;
    r.connector[0].edid_bytes = 256;
    r.connector[0].preferred_width = 2560;
    r.connector[0].preferred_height = 1440;
    r.connector[0].preferred_refresh = 60;
    r.connector[0].lit = 1;
    r.connector[0].lit_width = 2560;
    r.connector[0].lit_height = 1440;
    r.connector[0].lit_refresh = 60;
    strcpy(r.connector[1].name, "HDMI-A-1");
    r.connector[1].status = 2;
    return r;
}

static int monitor_lookup(const char *name, struct rt_display_monitor &m)
{
    if (!strcmp(name, "DP-4")) {
        m.status = 1;
        m.width_mm = 597;
        m.height_mm = 336;
        strcpy(m.name, "DELL UP2716D");
        return 0;
    }
    if (!strcmp(name, "HDMI-A-1")) {
        m.status = 2;
        return 0;
    }
    return -2;
}

static void test_displays()
{
    const struct rt_display_report report = two_connectors();
    DisplayState state;
    display_state(report, monitor_lookup, state);
    assert(state.epoch == 3 && state.count == 2);
    assert(!strcmp(state.connector[0].monitor, "DELL UP2716D") && state.connector[0].widthMM == 597);
    assert(!state.connector[1].monitor[0]);

    DisplayState again;
    display_state(report, monitor_lookup, again);
    assert(display_state_equal(state, again));
    struct rt_display_report unplugged = report;
    unplugged.connector[0].status = 2;
    unplugged.hotplug_epoch = 4;
    display_state(unplugged, [](const char *, struct rt_display_monitor &) { return -19; }, again);
    assert(!display_state_equal(state, again) && !again.connector[0].monitor[0]);

    OSDictionary *p = display_properties(state);
    assert(p && p->getCount() == 1);
    auto *displays = dynamic_cast<const OSDictionary *>(p->getObject(kMLGDisplays));
    assert(displays && number_is(displays, kMLGDisplaysEpoch, 3, 32));
    auto *connectors = dynamic_cast<const OSArray *>(displays->getObject(kMLGDisplaysConnectors));
    assert(connectors && connectors->getCount() == 2);
    auto *dp = dynamic_cast<const OSDictionary *>(connectors->getObject(0));
    assert(dp && string_is(dp, kMLGConnectorName, "DP-4") &&
           string_is(dp, kMLGConnectorStatus, "connected") &&
           string_is(dp, kMLGConnectorMonitor, "DELL UP2716D") &&
           number_is(dp, kMLGConnectorWidthMM, 597, 32) && number_is(dp, kMLGConnectorHeightMM, 336, 32) &&
           number_is(dp, kMLGConnectorPreferredWidth, 2560, 32) &&
           number_is(dp, kMLGConnectorPreferredHeight, 1440, 32) &&
           number_is(dp, kMLGConnectorPreferredRefresh, 60, 32) &&
           dp->getObject(kMLGConnectorLit) == kOSBooleanTrue &&
           number_is(dp, kMLGConnectorLitWidth, 2560, 32) &&
           number_is(dp, kMLGConnectorLitHeight, 1440, 32) &&
           number_is(dp, kMLGConnectorLitRefresh, 60, 32));
    auto *hdmi = dynamic_cast<const OSDictionary *>(connectors->getObject(1));
    assert(hdmi && string_is(hdmi, kMLGConnectorName, "HDMI-A-1") &&
           string_is(hdmi, kMLGConnectorStatus, "disconnected") &&
           !hdmi->getObject(kMLGConnectorMonitor) && !hdmi->getObject(kMLGConnectorWidthMM) &&
           !hdmi->getObject(kMLGConnectorPreferredWidth) && !hdmi->getObject(kMLGConnectorLitWidth) &&
           hdmi->getObject(kMLGConnectorLit) == kOSBooleanFalse);
    p->release();
    assert(mock_live_objects == 0);

    /* No connectors: an empty list, still published. */
    DisplayState none;
    memset(&none, 0, sizeof(none));
    p = display_properties(none);
    displays = p ? dynamic_cast<const OSDictionary *>(p->getObject(kMLGDisplays)) : nullptr;
    connectors = displays ? dynamic_cast<const OSArray *>(displays->getObject(kMLGDisplaysConnectors)) : nullptr;
    assert(connectors && connectors->getCount() == 0);
    p->release();
    assert(mock_live_objects == 0);
}

/* Every allocation, failed in turn: NULL and nothing left behind. */
template <typename Build>
static void test_failures(const char *what, Build build)
{
    mock_allocations = 0;
    mock_fail_at = 0;
    OSObject *whole = build();
    assert(whole);
    whole->release();
    const long allocations = mock_allocations;
    assert(allocations > 0);
    for (long at = 1; at <= allocations; ++at) {
        mock_allocations = 0;
        mock_fail_at = at;
        OSObject *p = build();
        assert(!p);
        assert(mock_live_objects == 0);
    }
    mock_fail_at = 0;
    printf("  %s: %ld allocation failures, each returns NULL, no leaks\n", what, allocations);
}

int main()
{
    test_helpers();
    test_identity_at_start();
    test_identity_probed();
    test_displays();
    test_failures("identity at start", [] { return (OSObject *)identity_properties(r9700_at_start()); });
    test_failures("identity probed", [] { return (OSObject *)identity_properties(r9700_probed()); });
    test_failures("displays", [] {
        DisplayState state;
        display_state(two_connectors(), monitor_lookup, state);
        return (OSObject *)display_properties(state);
    });
    printf("PASS device-properties: System Information keys (model, VRAM,totalMB, ATY,EFIVersionB, "
           "rom-revision), MacLinuxGPUDevice, MacLinuxGPUDisplays, allocation failures\n");
    return 0;
}
