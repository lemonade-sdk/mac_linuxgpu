/* The display output on the fixture device (scanout_fixture.c) and what its
 * clients (libmlg_drm/libdrm/tests/test_scanout.c, vulkan/tests/
 * test_radv_scanout.c) ask of it. No kernel types. */
#ifndef TEST_SCANOUT_H
#define TEST_SCANOUT_H

#include <stdint.h>

struct pci_dev;

/* The fixture device with its display, flip interrupts running, an output
 * lit on HDMI-A-1 at 1920x1080 and a desktop frame of @desktop on it. */
struct pci_dev *scanout_fixture_start(uint32_t desktop);
/* The output off, the desktop surface removed, the device stopped. */
void scanout_fixture_finish(void);

/* The libdrm-mlg client's checks; 0 on success. */
int test_scanout(void);

/* The value the client leaves on the primary plane at its end (the main
 * program then checks that the client's death takes it off). */
#define TEST_SCANOUT_LEFT_ON_SCREEN	0x00c0ffeeu

/* Stop or resume the fixture's vblank (flip) interrupts. */
void scanout_fx_hold_vblank(int hold);
/* The 32-bit pixel at byte @offset of the surface pipe @pipe scans out. */
uint32_t scanout_fx_pipe_pixel(unsigned int pipe, uint64_t offset);
/* A bit per enabled pipe whose surface starts with @value. */
unsigned int scanout_fx_pipes_showing(uint32_t value);
/* Whether the DRM device has a master. */
int scanout_fx_has_master(void);
/* A whole desktop frame of @value, presented as a display agent does. */
int scanout_fx_desktop_present(uint32_t value);
void scanout_fx_desktop_stats(uint64_t *received, uint64_t *flipped, int *error);
/* The output off and lit again (rt_display_off, rt_display_output). */
int scanout_fx_relight(void);
/* LX_SCANOUT ATTACH from a second client: its result. */
int scanout_fx_second_client_attach(void);

#endif
