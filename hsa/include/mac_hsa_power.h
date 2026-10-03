#pragma once
// Device power for runtime clients (the driver's side and the upstream
// paths behind each state: dext/sources/power_state.h in mac_linuxgpu).
//
// A GPU agent is active, suspending, suspended, resuming or lost.
//  - Suspended with MAC_HSA_POWER_FLAG_VRAM_PRESERVED: the device is idle
//    with every queue unmapped and its work saved; buffers keep their
//    contents. Doorbells rung meanwhile wait in the runtime and are rung on
//    resume, so work in flight pauses and continues. Calls that would put
//    new work on the GPU (allocation, transfers, queue creation, bounded
//    launches) fail with MAC_HSA_STATUS_SUSPENDED having done nothing;
//    retry them after resume.
//  - Lost: the host slept and the device's memory went with it (or a power
//    transition failed). Every buffer, queue and executable of the session
//    is gone; queue error callbacks receive MAC_HSA_STATUS_DEVICE_LOST. To
//    continue, release everything, hsa_shut_down, hsa_init (which probes
//    the device afresh) and load again (for an inference engine: reload the
//    model).
//
// When to call: before the app stops using the GPU for a while (iPadOS
// willResignActive / didEnterBackground; macOS NSWorkspace willSleep),
// mac_hsa_agent_prepare_low_power; when it comes back (didBecomeActive,
// didWake), mac_hsa_agent_resume. Neither is required for correctness: the
// driver handles host sleep on its own; preparing first lets work finish
// or pause cleanly and the client learn early.
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MAC_HSA_POWER_ACTIVE = 0,
    MAC_HSA_POWER_SUSPENDING = 1,
    MAC_HSA_POWER_SUSPENDED = 2,
    MAC_HSA_POWER_RESUMING = 3,
    MAC_HSA_POWER_LOST = 4,
};
enum {
    MAC_HSA_POWER_FLAG_VRAM_PRESERVED = 1u << 0, // device memory is what the session left
    MAC_HSA_POWER_FLAG_SYSTEM_SLEEP = 1u << 1,   // the host is going to sleep
    MAC_HSA_POWER_FLAG_DEVICE_LOW = 1u << 2,     // the OS put the device in a low power state
    MAC_HSA_POWER_FLAG_CLIENT_HOLD = 1u << 3,    // some client prepared for low power
    MAC_HSA_POWER_FLAG_QUIESCED = 1u << 4,       // compute queues are unmapped, work saved
    MAC_HSA_POWER_FLAG_SESSION_CLOSED = 1u << 5, // the session was closed for a sleep
    MAC_HSA_POWER_FLAG_ACK_PENDING = 1u << 6,
    MAC_HSA_POWER_FLAG_LINK_DOWN = 1u << 7,      // the device did not answer after wake
};
// Retry after resume: nothing was submitted.
#define MAC_HSA_STATUS_SUSPENDED ((hsa_status_t)HSA_STATUS_ERROR_RESOURCE_BUSY)
// The device's memory is gone: reload (see above).
#define MAC_HSA_STATUS_DEVICE_LOST ((hsa_status_t)HSA_STATUS_ERROR_FATAL)

typedef struct mac_hsa_power_state_s {
    uint64_t version;            // 1
    uint32_t state;              // MAC_HSA_POWER_*
    uint32_t flags;              // MAC_HSA_POWER_FLAG_*
    uint64_t generation;         // changes on every state change
    uint32_t cause;              // the driver's cause of the last transition
    int32_t error;               // the driver's error of the last failed step
    uint32_t holds;              // clients holding a low-power request
    uint32_t paused_queues;      // this process's queues holding a doorbell
    uint64_t quiesces;           // low-power periods that kept device memory
    uint64_t losses;             // transitions that lost device memory
    uint64_t last_transition_us; // duration of the last completed transition
} mac_hsa_power_state_t;

// The agent's power state. HSA_STATUS_ERROR_INVALID_ARGUMENT from a driver
// that predates it.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_get_power_state(hsa_agent_t agent, mac_hsa_power_state_t *state,
                                           size_t state_size);

// Ask for low power. The runtime stops ringing doorbells for this agent's
// queues (they wait and are rung on resume), waits up to drain_timeout_ms
// for the packets already rung to finish, then asks the driver, which
// unmaps every queue (saving work still running) and refuses new work until
// the last client resumes. Returns once the driver finished; *state (may be
// NULL) receives the state after it.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_prepare_low_power(hsa_agent_t agent, uint32_t drain_timeout_ms,
                                             mac_hsa_power_state_t *state, size_t state_size);

// Drop this process's low-power request; when no client holds one the
// driver maps the queues back, and the doorbells that waited are rung.
// MAC_HSA_STATUS_DEVICE_LOST when the device's memory is gone.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_resume(hsa_agent_t agent, mac_hsa_power_state_t *state, size_t state_size);

// Wait up to timeout_ms for the agent's power generation to differ from
// known_generation; *state receives the state either way (compare its
// generation). Polls the driver's cached state; never touches the GPU.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_wait_power_state(hsa_agent_t agent, uint64_t known_generation,
                                            uint32_t timeout_ms, mac_hsa_power_state_t *state,
                                            size_t state_size);

#ifdef __cplusplus
}
#endif
