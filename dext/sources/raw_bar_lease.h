#pragma once
#include <stdint.h>

namespace maclinuxgpu {
/* Raw mappings permit writes outside RPC serialization, so their lease
 * lasts until client retirement. Claimed on the delivery thread (a client's
 * memory mapping) and checked and released on the session queue, so every
 * operation takes the lease's own spinlock (held for a few loads and
 * stores). */
class RawBARLease {
    uint64_t owner = 0;
    bool mapped = false;
    uint32_t lock = 0;
    void acquire() { while (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) {} }
    void release_lock() { __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE); }
    bool allowsJoinLocked(uint64_t client) const { return client && (!owner || owner == client); }
public:
    bool allowsJoin(uint64_t client) {
        acquire();
        const bool allowed = allowsJoinLocked(client);
        release_lock();
        return allowed;
    }
    bool claim(uint64_t client, bool attached, uint32_t participants) {
        acquire();
        const bool claimed = attached && participants == 1 && allowsJoinLocked(client);
        if (claimed) owner = client;
        release_lock();
        return claimed;
    }
    bool markMapped(uint64_t client) {
        acquire();
        const bool marked = client && owner == client;
        if (marked) mapped = true;
        release_lock();
        return marked;
    }
    bool hasMappings() {
        acquire();
        const bool has = mapped;
        release_lock();
        return has;
    }
    void release(uint64_t client) {
        acquire();
        if (owner == client) { owner = 0; mapped = false; }
        release_lock();
    }
};
}
