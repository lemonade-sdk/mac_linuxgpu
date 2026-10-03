#ifndef MAC_LINUXGPU_PCI_ACCESS_GATE_H
#define MAC_LINUXGPU_PCI_ACCESS_GATE_H

/* Admission closes before isolation. An operation that raced the first
 * check must withdraw before touching the provider; an admitted operation
 * remains counted until its final RPC completes. */
class dext_pci_access_gate {
    bool blocked = false;
    unsigned active = 0;
public:
    bool enter() {
        if (__atomic_load_n(&blocked, __ATOMIC_SEQ_CST)) return false;
        __atomic_add_fetch(&active, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&blocked, __ATOMIC_SEQ_CST)) return true;
        leave();
        return false;
    }
    void leave() { __atomic_sub_fetch(&active, 1, __ATOMIC_SEQ_CST); }
    void block() { __atomic_store_n(&blocked, true, __ATOMIC_SEQ_CST); }
    /* Reopen only after every admitted operation has left; the owner must
     * also have proven that nothing can race the reopening. */
    bool reopen() {
        if (!drained()) return false;
        __atomic_store_n(&blocked, false, __ATOMIC_SEQ_CST);
        return true;
    }
    bool closed() const { return __atomic_load_n(&blocked, __ATOMIC_SEQ_CST); }
    bool drained() const { return __atomic_load_n(&active, __ATOMIC_SEQ_CST) == 0; }
};
#endif
