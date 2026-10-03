/* The driver's device power state machine (dext/sources/power_state.h):
 * its transitions, the snapshot clients read, which selectors it admits in
 * each state, and what each power event plans from cached state; plus the
 * runtime's copy of the protocol (hsa/abi/amdgpu_power_abi.h) agreeing
 * with the driver's. */
#include "power_state.h"
#include "session_state.h"
#include "../../hsa/abi/amdgpu_power_abi.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace pw = amdgpu::power;

static_assert(pw::kQueryTag == MLG_QUERY_POWER_STATE);
static_assert(pw::kSelector == MLG_SELECTOR_POWER);
static_assert(pw::kVersion == MLG_POWER_STATE_VERSION && pw::kWords == MLG_POWER_STATE_WORDS);
static_assert(uint32_t(pw::State::Active) == MLG_POWER_ACTIVE &&
              uint32_t(pw::State::Suspending) == MLG_POWER_SUSPENDING &&
              uint32_t(pw::State::Suspended) == MLG_POWER_SUSPENDED &&
              uint32_t(pw::State::Resuming) == MLG_POWER_RESUMING &&
              uint32_t(pw::State::Lost) == MLG_POWER_LOST);
#define SAME(a, b) (uint64_t(a) == uint64_t(b))
static_assert(SAME(pw::VRAMPreserved, MLG_POWER_FLAG_VRAM_PRESERVED) &&
              SAME(pw::SystemSleep, MLG_POWER_FLAG_SYSTEM_SLEEP) &&
              SAME(pw::DeviceLow, MLG_POWER_FLAG_DEVICE_LOW) &&
              SAME(pw::ClientHold, MLG_POWER_FLAG_CLIENT_HOLD) &&
              SAME(pw::KFDQuiesced, MLG_POWER_FLAG_KFD_QUIESCED) &&
              SAME(pw::SessionClosed, MLG_POWER_FLAG_SESSION_CLOSED) &&
              SAME(pw::AckPending, MLG_POWER_FLAG_ACK_PENDING) &&
              SAME(pw::LinkDown, MLG_POWER_FLAG_LINK_DOWN));
static_assert(SAME(pw::Query, MLG_POWER_OP_QUERY) && SAME(pw::Prepare, MLG_POWER_OP_PREPARE) &&
              SAME(pw::Resume, MLG_POWER_OP_RESUME) && SAME(pw::Wait, MLG_POWER_OP_WAIT));
static_assert(pw::Generation == 2 && pw::Flags == 3 && pw::SessionGeneration == 10);
/* Outside every existing selector and query namespace. */
static_assert(MLG_SELECTOR_POWER > MLG_SELECTOR_DRM_SELFTEST);
static_assert(MLG_QUERY_POWER_STATE != MLG_QUERY_SESSION_STATE &&
              MLG_QUERY_POWER_STATE != MLG_QUERY_PROBE_STATUS &&
              MLG_QUERY_POWER_STATE != MLG_QUERY_KERNEL_LOG);

static void transitions()
{
    const uint32_t all[] = {MLG_POWER_ACTIVE, MLG_POWER_SUSPENDING, MLG_POWER_SUSPENDED,
                            MLG_POWER_RESUMING, MLG_POWER_LOST};
    unsigned allowed = 0;
    for (uint32_t from : all)
        for (uint32_t to : all)
            allowed += mlg_power_transition_allowed(from, to);
    assert(allowed == 12);
    assert(!mlg_power_transition_allowed(MLG_POWER_ACTIVE, MLG_POWER_SUSPENDED));
    assert(!mlg_power_transition_allowed(MLG_POWER_ACTIVE, MLG_POWER_RESUMING));
    assert(!mlg_power_transition_allowed(MLG_POWER_SUSPENDED, MLG_POWER_ACTIVE));
    assert(!mlg_power_transition_allowed(MLG_POWER_RESUMING, MLG_POWER_SUSPENDED));
    assert(!mlg_power_transition_allowed(MLG_POWER_LOST, MLG_POWER_SUSPENDED));
    assert(!mlg_power_transition_allowed(7, MLG_POWER_ACTIVE));

    struct mlg_power p;
    mlg_power_init(&p);
    assert(p.state == MLG_POWER_ACTIVE && p.generation == 1);
    assert(p.flags == MLG_POWER_FLAG_VRAM_PRESERVED);
    /* Quiesce and resume: VRAM kept, four generations. */
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    assert(!mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    assert(mlg_power_set(&p, MLG_POWER_RESUMING, MLG_POWER_CAUSE_CLIENT_RESUME, 0));
    assert(mlg_power_set(&p, MLG_POWER_ACTIVE, MLG_POWER_CAUSE_CLIENT_RESUME, 0));
    assert(p.generation == 5 && (p.flags & MLG_POWER_FLAG_VRAM_PRESERVED) && !p.losses);
    /* An illegal step changes nothing. */
    assert(!mlg_power_set(&p, MLG_POWER_RESUMING, MLG_POWER_CAUSE_DEVICE_ON, -5));
    assert(p.generation == 5 && p.state == MLG_POWER_ACTIVE && p.error == 0);
    /* A sleep: suspending, then the closed session is lost on wake. */
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0));
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0));
    assert(mlg_power_set(&p, MLG_POWER_LOST, MLG_POWER_CAUSE_SYSTEM_WAKE, 0));
    assert(!(p.flags & MLG_POWER_FLAG_VRAM_PRESERVED) && p.losses == 1);
    /* A re-probe makes it active again; a failure keeps its error. */
    assert(mlg_power_set(&p, MLG_POWER_ACTIVE, MLG_POWER_CAUSE_REPROBED, 0));
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    assert(mlg_power_set(&p, MLG_POWER_LOST, MLG_POWER_CAUSE_QUIESCE_FAILED, -5));
    assert(p.error == -5 && p.losses == 2 && p.cause == MLG_POWER_CAUSE_QUIESCE_FAILED);
    /* A sleep while lost has nothing to close. */
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0));
}

static void snapshot()
{
    struct mlg_power p;
    uint64_t out[MLG_POWER_STATE_WORDS];
    mlg_power_init(&p);
    p.holds = 2;
    p.quiesces = 3;
    p.last_us = 1234;
    p.session = 9;
    p.error = -110;
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_DEVICE_LOW, 0));
    memset(out, 0xa5, sizeof(out));
    mlg_power_snapshot(&p, out);
    assert(out[pw::Version] == 1 && out[pw::State] == MLG_POWER_SUSPENDING);
    assert(out[pw::Generation] == 2);
    assert(out[pw::Flags] == (MLG_POWER_FLAG_VRAM_PRESERVED | MLG_POWER_FLAG_CLIENT_HOLD));
    assert(out[pw::Cause] == MLG_POWER_CAUSE_DEVICE_LOW);
    assert(int64_t(out[pw::Error]) == -110);
    assert(out[pw::Holds] == 2 && out[pw::Quiesces] == 3 && out[pw::Losses] == 0);
    assert(out[pw::LastTransitionMicroseconds] == 1234 && out[pw::SessionGeneration] == 9);
    assert(out[pw::Reserved] == 0);
}

static void admission()
{
    /* Active and lost admit everything the session itself allows. */
    for (uint64_t selector = 0; selector < 100; ++selector) {
        assert(mlg_power_admits(MLG_POWER_ACTIVE, selector));
        assert(mlg_power_admits(MLG_POWER_LOST, selector));
    }
    const uint32_t closed[] = {MLG_POWER_SUSPENDING, MLG_POWER_SUSPENDED, MLG_POWER_RESUMING};
    /* Work for the GPU waits for resume. */
    const uint64_t refused[] = {1 /* GetIdentity */, 9 /* InitDevice */, 16 /* BOAlloc */, 19 /* SubmitIB */,
        20 /* WaitFence */, 44 /* HostMemoryTest */, 48, 49, 50 /* BO copy/write/read */,
        51 /* ComputeDispatch */, 52, 53 /* BO export/import */, 54 /* HostWindow */, 55 /* AQLDispatch */,
        56 /* AQLQueueCreate */, 57 /* AQLQueueKick */, 59 /* AQLQueueService */};
    /* Releasing, cached state and the power selector itself do not. */
    const uint64_t admitted[] = {0, 17, 18, 21, 36, 42, 43, 58, 61, MLG_SELECTOR_POWER};
    for (uint32_t state : closed) {
        for (uint64_t selector : refused) assert(!mlg_power_admits(state, selector));
        for (uint64_t selector : admitted) assert(mlg_power_admits(state, selector));
    }
}

static void plans()
{
    struct mlg_power p;
    mlg_power_init(&p);
    /* A first PREPARE quiesces an open session, or only idles. */
    p.holds = 1;
    assert(mlg_power_plan_hold(&p, true) == MLG_POWER_DO_QUIESCE);
    assert(mlg_power_plan_hold(&p, false) == MLG_POWER_DO_IDLE);
    p.holds = 2; /* a second holder while still active (the first failed) */
    assert(mlg_power_plan_hold(&p, true) == MLG_POWER_DO_QUIESCE);
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    p.flags |= MLG_POWER_FLAG_KFD_QUIESCED;
    assert(mlg_power_plan_hold(&p, true) == MLG_POWER_DO_NOTHING);
    /* Only the last RESUME resumes. */
    p.holds = 1;
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_NOTHING);
    p.holds = 0;
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_RESUME);
    /* Not while the device is reported low or the host asleep. */
    p.flags |= MLG_POWER_FLAG_DEVICE_LOW;
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_NOTHING);
    p.flags &= ~MLG_POWER_FLAG_DEVICE_LOW;
    /* Device low while active quiesces; On resumes once nobody holds. */
    struct mlg_power q;
    mlg_power_init(&q);
    assert(mlg_power_plan_capability(&q, MLG_POWER_CAPABILITY_LOW, true) == MLG_POWER_DO_QUIESCE);
    assert(mlg_power_plan_capability(&q, MLG_POWER_CAPABILITY_LOW, false) == MLG_POWER_DO_IDLE);
    assert(mlg_power_plan_capability(&q, MLG_POWER_CAPABILITY_ON, true) == MLG_POWER_DO_NOTHING);
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_ON, true) == MLG_POWER_DO_RESUME);
    p.holds = 1;
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_ON, true) == MLG_POWER_DO_NOTHING);
    p.holds = 0;
    /* Sleep closes an open session whatever the state; without one it
     * only idles an active device. */
    assert(mlg_power_plan_capability(&q, MLG_POWER_CAPABILITY_OFF, true) == MLG_POWER_DO_CLOSE_SESSION);
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_OFF, true) == MLG_POWER_DO_CLOSE_SESSION);
    assert(mlg_power_plan_capability(&q, MLG_POWER_CAPABILITY_OFF, false) == MLG_POWER_DO_IDLE);
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_OFF, false) == MLG_POWER_DO_NOTHING);
    /* Wake after the close: lost; after an idle sleep: active again. */
    p.flags |= MLG_POWER_FLAG_SESSION_CLOSED;
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_ON, false) == MLG_POWER_DO_WAKE_LOST);
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_NOTHING);
    p.flags &= ~(MLG_POWER_FLAG_SESSION_CLOSED | MLG_POWER_FLAG_KFD_QUIESCED);
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_ON, false) == MLG_POWER_DO_WAKE_IDLE);
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_WAKE_IDLE);
}

/* The sequences the driver runs, as state, for the cases the report lists. */
static void sequences()
{
    struct mlg_power p;
    uint64_t out[MLG_POWER_STATE_WORDS];
    mlg_power_init(&p);
    /* iPad app to the background and back: quiesce, resume, VRAM kept. */
    p.holds = 1;
    assert(mlg_power_plan_hold(&p, true) == MLG_POWER_DO_QUIESCE);
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    p.flags |= MLG_POWER_FLAG_KFD_QUIESCED;
    ++p.quiesces;
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    p.holds = 0;
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_RESUME);
    assert(mlg_power_set(&p, MLG_POWER_RESUMING, MLG_POWER_CAUSE_CLIENT_RESUME, 0));
    p.flags &= ~MLG_POWER_FLAG_KFD_QUIESCED;
    assert(mlg_power_set(&p, MLG_POWER_ACTIVE, MLG_POWER_CAUSE_CLIENT_RESUME, 0));
    mlg_power_snapshot(&p, out);
    assert(out[pw::State] == MLG_POWER_ACTIVE && (out[pw::Flags] & pw::VRAMPreserved));
    assert(out[pw::Quiesces] == 1 && out[pw::Losses] == 0 && out[pw::Generation] == 5);

    /* Mac sleep with a model loaded and a PREPARE from the host app: the
     * quiesced session is resumed, closed, and found lost on wake; a new
     * InitDevice makes it active. */
    p.holds = 1;
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    p.flags |= MLG_POWER_FLAG_KFD_QUIESCED;
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_CLIENT_PREPARE, 0));
    p.flags |= MLG_POWER_FLAG_SYSTEM_SLEEP;
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_OFF, true) == MLG_POWER_DO_CLOSE_SESSION);
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0));
    p.flags &= ~MLG_POWER_FLAG_KFD_QUIESCED;
    p.flags |= MLG_POWER_FLAG_SESSION_CLOSED;
    p.flags &= ~MLG_POWER_FLAG_VRAM_PRESERVED;
    assert(mlg_power_set(&p, MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0));
    p.flags &= ~MLG_POWER_FLAG_SYSTEM_SLEEP;
    assert(mlg_power_plan_capability(&p, MLG_POWER_CAPABILITY_ON, false) == MLG_POWER_DO_WAKE_LOST);
    assert(mlg_power_set(&p, MLG_POWER_LOST, MLG_POWER_CAUSE_SYSTEM_WAKE, 0));
    p.flags &= ~MLG_POWER_FLAG_SESSION_CLOSED;
    mlg_power_snapshot(&p, out);
    assert(out[pw::State] == MLG_POWER_LOST && !(out[pw::Flags] & pw::VRAMPreserved));
    assert(out[pw::Losses] == 1);
    /* The holder resumes into a lost session: nothing to do. */
    p.holds = 0;
    assert(mlg_power_plan_release(&p) == MLG_POWER_DO_NOTHING);
    assert(mlg_power_set(&p, MLG_POWER_ACTIVE, MLG_POWER_CAUSE_REPROBED, 0));
    p.flags |= MLG_POWER_FLAG_VRAM_PRESERVED;
    mlg_power_snapshot(&p, out);
    assert(out[pw::State] == MLG_POWER_ACTIVE && out[pw::Cause] == MLG_POWER_CAUSE_REPROBED);
}

int main()
{
    transitions();
    snapshot();
    admission();
    plans();
    sequences();
    puts("device power state: transitions, snapshot, admission, plans and the runtime's protocol copy passed");
    return 0;
}
