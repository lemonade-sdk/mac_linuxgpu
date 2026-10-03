#ifndef MAC_LINUXGPU_OBSERVER_GATE_H
#define MAC_LINUXGPU_OBSERVER_GATE_H

/* Admission for observer reads that run upstream callbacks (sysfs show(),
 * AMDGPU_INFO) on an observer client's own queue, concurrently with the
 * session queue. The session queue opens admission once the upstream
 * driver runs in an open session, and closes it before any step that can
 * remove the driver: close() then wait for drained(). A read enters before
 * it looks at session state and leaves after its last callback returns, so
 * either it sees admission closed or the closer sees it counted. Starts
 * closed. */
class mlg_observer_gate {
    bool open_ = false;
    unsigned active_ = 0;
public:
    bool enter() {
        __atomic_add_fetch(&active_, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&open_, __ATOMIC_SEQ_CST)) return true;
        leave();
        return false;
    }
    void leave() { __atomic_sub_fetch(&active_, 1, __ATOMIC_SEQ_CST); }
    void open() { __atomic_store_n(&open_, true, __ATOMIC_SEQ_CST); }
    void close() { __atomic_store_n(&open_, false, __ATOMIC_SEQ_CST); }
    bool admitting() const { return __atomic_load_n(&open_, __ATOMIC_SEQ_CST); }
    bool drained() const { return __atomic_load_n(&active_, __ATOMIC_SEQ_CST) == 0; }
};

#endif
