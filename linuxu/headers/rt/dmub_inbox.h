/* Compile intervention for drivers/gpu/drm/amd/display/dmub/src/dmub_srv.c
 * (patches/manifest.json): DMUB's inbox ring lives in VRAM, and two of
 * dmub_cmd.h's inline ring helpers touch it with plain pointer accesses,
 * a byte loop in dmub_rb_push_front and READ_ONCE in dmub_rb_flush_pending.
 * Through a CPU mapping of the BAR, the push is the store that panicked the
 * Mac when the GPU was unplugged (builds 239 and 240). Here dmub_cmd.h is
 * included first, as dmub_srv.c would include it, and dmub_srv.c's calls
 * are redirected to the same operations written with dmub_memcpy, as
 * upstream's own dmub_rb_out_push_front copies: memcpy reaches VRAM only
 * through the kernel (rt/device_string.h). The ring arithmetic is
 * unchanged. */
#ifndef LINUXU_RT_DMUB_INBOX_H
#define LINUXU_RT_DMUB_INBOX_H
#include <rt/device_string.h>	/* memcpy is the aperture's before these bodies */
#include "dmub/dmub_srv.h"

static inline bool linuxu_dmub_rb_push_front(struct dmub_rb *rb, const union dmub_rb_cmd *cmd)
{
	uint8_t *dst = (uint8_t *)(rb->base_address) + rb->wrpt;

	if (rb->capacity == 0)
		return false;
	if (dmub_rb_full(rb))
		return false;
	dmub_memcpy(dst, cmd, DMUB_RB_CMD_SIZE);
	rb->wrpt += DMUB_RB_CMD_SIZE;
	if (rb->wrpt >= rb->capacity)
		rb->wrpt %= rb->capacity;
	return true;
}

/* Read back every queued command, so the writes have reached VRAM before
 * the DMCUB is told; each read goes through the kernel as the writes did. */
static inline void linuxu_dmub_rb_flush_pending(const struct dmub_rb *rb)
{
	uint32_t rptr = rb->rptr;
	uint32_t wptr = rb->wrpt;
	uint64_t data[DMUB_RB_CMD_SIZE / sizeof(uint64_t)];

	if (rb->capacity == 0)
		return;
	while (rptr != wptr) {
		dmub_memcpy(data, (uint8_t *)(rb->base_address) + rptr, DMUB_RB_CMD_SIZE);
		rptr += DMUB_RB_CMD_SIZE;
		if (rptr >= rb->capacity)
			rptr %= rb->capacity;
	}
}

#define dmub_rb_push_front(rb, cmd) linuxu_dmub_rb_push_front(rb, cmd)
#define dmub_rb_flush_pending(rb) linuxu_dmub_rb_flush_pending(rb)
#endif
