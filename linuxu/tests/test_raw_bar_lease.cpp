#include "../../dext/sources/raw_bar_lease.h"
#include <cassert>
#include <cstdio>

int main()
{
    maclinuxgpu::RawBARLease lease;
    assert(!lease.allowsJoin(0));
    assert(lease.allowsJoin(1) && lease.allowsJoin(2));
    assert(!lease.claim(1, false, 1));
    assert(!lease.claim(1, true, 0));
    assert(!lease.claim(1, true, 2));
    assert(lease.allowsJoin(2));

    // Claim precedes descriptor creation. Even a failed mapping attempt
    // retains exclusivity, matching the reference session contract.
    assert(lease.claim(1, true, 1));
    assert(!lease.hasMappings());
    assert(lease.allowsJoin(1) && !lease.allowsJoin(2));
    assert(!lease.claim(2, true, 1));
    assert(!lease.markMapped(2));
    lease.release(2);
    assert(!lease.allowsJoin(2));
    lease.release(1);
    assert(lease.allowsJoin(2));

    // Successful mapping blocks later joins, not just concurrent mappers.
    assert(lease.claim(1, true, 1) && lease.markMapped(1));
    assert(lease.hasMappings());
    assert(lease.claim(1, true, 1));
    assert(!lease.allowsJoin(2) && !lease.claim(2, true, 2));
    lease.release(2);
    assert(lease.hasMappings());
    lease.release(1);
    assert(!lease.hasMappings() && lease.allowsJoin(2));
    assert(lease.claim(2, true, 1) && lease.markMapped(2));
    assert(!lease.allowsJoin(1));
    lease.release(2);
    assert(!lease.hasMappings());
    std::puts("PASS raw BAR lease: sole participant, persistent peer exclusion, mapping/reset guard and owner-only retirement");
}
