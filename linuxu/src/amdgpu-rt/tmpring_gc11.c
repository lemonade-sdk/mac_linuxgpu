/* COMPUTE_TMPRING_SIZE field layout of the family upstream drives with
 * gfx_v11_0.c, from the gc register header that file builds with. */
#include <rt/queue.h>
#include "gc/gc_11_0_0_sh_mask.h"

const struct rt_tmpring_layout rt_tmpring_gc11 = {
    .waves_mask = COMPUTE_TMPRING_SIZE__WAVES_MASK,
    .waves_shift = COMPUTE_TMPRING_SIZE__WAVES__SHIFT,
    .wave_size_mask = COMPUTE_TMPRING_SIZE__WAVESIZE_MASK,
    .wave_size_shift = COMPUTE_TMPRING_SIZE__WAVESIZE__SHIFT,
};
