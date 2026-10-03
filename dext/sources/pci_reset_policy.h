#ifndef MAC_LINUXGPU_PCI_RESET_POLICY_H
#define MAC_LINUXGPU_PCI_RESET_POLICY_H
#include <stdint.h>

/* Isolation for an uncertain live device. No reset, BAR relocation, or
 * backing release is permitted. Keep memory decode and descriptors intact
 * for existing CPU mappings while disabling new device DMA requests. */
template <typename PCI, typename Sleep>
static int dext_isolate_endpoint(PCI &pci, Sleep sleep)
{
    uint16_t vendor = UINT16_MAX, command = UINT16_MAX;
    pci.ConfigurationRead16(0, &vendor);
    pci.ConfigurationRead16(4, &command);
    if (vendor != 0x1002 || command == UINT16_MAX) return -19;
    if (command & 4) pci.ConfigurationWrite16(4, command & ~uint16_t(4));
    pci.ConfigurationRead16(4, &command);
    if (command == UINT16_MAX || (command & 4)) return -5;
    uint16_t status = UINT16_MAX;
    pci.ConfigurationRead16(6, &status);
    if (status == UINT16_MAX) return -19;
    if (!(status & 0x10)) return -95;
    uint8_t pos = 0xff, visited[256] = {};
    pci.ConfigurationRead8(0x34, &pos);
    unsigned express = 0;
    while (pos) {
        if (pos < 0x40 || pos > 0xfc || (pos & 3) || visited[pos]) return -5;
        visited[pos] = 1;
        uint8_t id = 0xff, next = 0xff;
        pci.ConfigurationRead8(pos, &id);
        pci.ConfigurationRead8(pos + 1, &next);
        if (id == 0xff) return -19;
        if (id == 0x10 && !express) express = pos;
        pos = next;
    }
    if (!express || express > 0xf4) return -95;
    for (unsigned elapsed = 0;; ++elapsed) {
        status = UINT16_MAX;
        pci.ConfigurationRead16(express + 0x0a, &status);
        if (status == UINT16_MAX) return -19;
        if (!(status & (1u << 5))) return 0;
        if (elapsed == 1000) return -110;
        sleep();
    }
}

/* Function reset for an otherwise idle endpoint. The owner must exclude new
 * DMA, client work, IRQ delivery and provider teardown for the entire call.
 * Reset preserves the host-assigned aperture; no bridge reset is permitted. */
template <typename PCI, typename Reset, typename Sleep>
static int dext_reset_endpoint(PCI &pci, Reset reset, Sleep sleep,
                              bool &configuration_changed, bool restore_command)
{
    configuration_changed = false;
    uint32_t identity = UINT32_MAX;
    uint16_t status = UINT16_MAX;
    pci.ConfigurationRead32(0, &identity);
    pci.ConfigurationRead16(6, &status);
    if ((identity & 0xffff) != 0x1002 || identity == UINT32_MAX ||
        status == UINT16_MAX) return -19;
    if (!(status & 0x10)) return -95;
    uint8_t pos = 0xff;
    pci.ConfigurationRead8(0x34, &pos);
    uint8_t visited[256] = {};
    unsigned express = 0;
    while (pos) {
        if (pos < 0x40 || pos > 0xfc || (pos & 3) || visited[pos]) return -5;
        visited[pos] = 1;
        uint8_t id = 0xff, next = 0xff;
        pci.ConfigurationRead8(pos, &id);
        pci.ConfigurationRead8(pos + 1, &next);
        if (id == 0xff) return -19;
        if (id == 0x10 && !express) express = pos;
        pos = next;
    }
    if (!express || express > 0xf4) return -95;
    uint32_t caps = UINT32_MAX, bars[6];
    pci.ConfigurationRead32(express + 4, &caps);
    if (caps == UINT32_MAX) return -19;
    if (!(caps & (1u << 28))) return -95;
    for (unsigned i = 0; i < 6; ++i) {
        bars[i] = UINT32_MAX;
        pci.ConfigurationRead32(0x10 + 4 * i, &bars[i]);
        if (bars[i] == UINT32_MAX) return -19;
    }
    uint16_t command = UINT16_MAX, after = UINT16_MAX;
    pci.ConfigurationRead16(4, &command);
    if (command == UINT16_MAX) return -19;
    if (command & 4) {
        configuration_changed = true;
        pci.ConfigurationWrite16(4, command & ~uint16_t(4));
    }
    pci.ConfigurationRead16(4, &after);
    if (after == UINT16_MAX || (after & 4)) return -5;
    unsigned elapsed = 0;
    for (;;) {
        status = UINT16_MAX;
        pci.ConfigurationRead16(express + 0x0a, &status);
        if (status == UINT16_MAX) return -19;
        if (!(status & (1u << 5))) break;
        if (elapsed++ == 1000) return -110;
        sleep();
    }
    configuration_changed = true;
    if (reset()) return -5;
    uint32_t observed = UINT32_MAX;
    pci.ConfigurationRead32(0, &observed);
    pci.ConfigurationRead16(4, &after);
    if (observed != identity || after == UINT16_MAX || (after & 4)) return -5;
    for (unsigned i = 0; i < 6; ++i) {
        observed = UINT32_MAX;
        pci.ConfigurationRead32(0x10 + 4 * i, &observed);
        if (observed != bars[i]) return -5;
    }
    if (!restore_command) return 0;
    /* The provider Reset restores its saved configuration. Restore the
     * original command only after confirming FLR and fixed BAR assignment. */
    if (after != command) pci.ConfigurationWrite16(4, command);
    pci.ConfigurationRead16(4, &after);
    return after == command ? 0 : -5;
}

template <typename PCI, typename Reset, typename Sleep>
static int dext_reset_idle_endpoint(PCI &pci, Reset reset, Sleep sleep,
                                    bool &configuration_changed)
{
    return dext_reset_endpoint(pci, reset, sleep, configuration_changed, true);
}

/* Shutdown never reenables bus mastering. The owner has stopped producers
 * and drained IRQ callbacks while retaining every retired DMA descriptor. */
template <typename PCI, typename Reset, typename Sleep>
static int dext_reset_shutdown_endpoint(PCI &pci, Reset reset, Sleep sleep,
                                        bool &configuration_changed)
{
    return dext_reset_endpoint(pci, reset, sleep, configuration_changed, false);
}
#endif
