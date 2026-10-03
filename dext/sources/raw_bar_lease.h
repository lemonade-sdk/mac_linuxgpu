#pragma once
#include <stdint.h>

namespace maclinuxgpu {
/* Serialized by the driver's lifecycle queue. Raw mappings permit writes
 * outside RPC serialization, so their lease lasts until client retirement. */
class RawBARLease {
    uint64_t owner = 0;
    bool mapped = false;
public:
    bool allowsJoin(uint64_t client) const {
        return client && (!owner || owner == client);
    }
    bool claim(uint64_t client, bool attached, uint32_t participants) {
        if (!attached || participants != 1 || !allowsJoin(client)) return false;
        owner = client;
        return true;
    }
    bool markMapped(uint64_t client) {
        if (!client || owner != client) return false;
        mapped = true;
        return true;
    }
    bool hasMappings() const { return mapped; }
    void release(uint64_t client) {
        if (owner == client) { owner = 0; mapped = false; }
    }
};
}
