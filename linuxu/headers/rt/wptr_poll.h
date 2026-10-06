/* The CP write-pointer polling experiment (latency option E).
 *
 * KFD's GFX 12 MQD already points CP_HQD_PQ_WPTR_POLL_ADDR at the queue's
 * write pointer (amd_queue_t.write_dispatch_id); upstream leaves the global
 * CP_PQ_WPTR_POLL_CNTL.EN off (gfx_v12_0_kiq_init_register) and MES takes
 * each queue's write-pointer address in ADD_QUEUE. With EN set the command
 * processor reads the write pointers itself every PERIOD, so a doorbell, a
 * store into the BAR this driver has to make for the client, is no longer
 * what starts the work: the client's store to its own write index is.
 *
 * Off unless the dext's personality sets MacLinuxGPUWptrPollPeriod (1-255,
 * the field's raw value; its unit is undocumented). Whatever the setting,
 * the register is read once after each KFD queue is created, when compute
 * is active (KFD keeps GFXOFF off) and MES has mapped the queue, and logged
 * whenever its value changed: that shows whether MES writes it on GFX 12.
 * GFX 12.0 only (the gc_12_0_0 register headers). */
#ifndef LINUXU_RT_WPTR_POLL_H
#define LINUXU_RT_WPTR_POLL_H

#include <stdbool.h>
#include <stdint.h>

struct amdgpu_device;

#ifdef __cplusplus
extern "C" {
#endif

/* @period 0: observe only. Called before the first session. */
void rt_wptr_poll_configure(uint32_t period);
/* After a KFD queue was created (rt_kfd_after_queue_create). */
void rt_wptr_poll_observe(struct amdgpu_device *adev);
/* Whether the CP polls write pointers with the configured period, as last
 * read back. */
bool rt_wptr_poll_active(void);

#ifdef __cplusplus
}
#endif

#endif
