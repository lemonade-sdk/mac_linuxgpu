/* The in-driver display test (linuxu/src/amdgpu-rt/display.c): an
 * in-kernel DRM client on the GPU's DRM device, as fbdev emulation is on
 * Linux, that shows a static test pattern on the GPU's own outputs through
 * upstream code only:
 *
 *   drm_client_init()                  a client file on the primary node
 *   drm_client_modeset_probe()         detect, EDID modes, preferred mode,
 *                                      one CRTC per connected connector
 *   drm_client_buffer_create_dumb()    an XRGB8888 dumb buffer (amdgpu
 *                                      places it in CPU-visible VRAM) and
 *                                      its framebuffer
 *   drm_client_buffer_vmap()           the pattern written through the BAR
 *   drm_client_modeset_commit()        the atomic commit (amdgpu_dm/DC)
 *
 * Before the first commit the current configuration is recorded: each
 * CRTC's mode and enable/active flags, each plane's CRTC, framebuffer and
 * rectangles, each connector's CRTC. Turning the pattern off commits that
 * configuration as one new atomic state (built like
 * drm_client_modeset_commit_atomic() builds one, so the driver checks it
 * against its current state), then deletes the framebuffer and releases
 * the client. A duplicated drm_atomic_state is not used: it would commit
 * amdgpu_dm's private DC context back stale. The client is never
 * registered, so it gets no hotplug callbacks and is never released by
 * upstream; rt_display_stop() releases it before the driver is removed.
 * A failing step is reported with its errno; no other mechanism is tried
 * in its place.
 *
 * Every wait is upstream's own bounded wait (flip_done and vblank waits
 * time out, DC's register and DMUB waits time out). Display must be
 * enabled (amdgpu.dc, rt/bootstrap.h); otherwise the device has no CRTC
 * and every call fails with -ENODEV.
 *
 * Shared with DriverKit and C++ callers: no kernel types. */
#ifndef LINUXU_RT_DISPLAY_H
#define LINUXU_RT_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;

#define RT_DISPLAY_VERSION		1u
#define RT_DISPLAY_CONNECTORS_MAX	8u
#define RT_DISPLAY_NAME_BYTES		32u

enum rt_display_pattern {
	RT_DISPLAY_PATTERN_BARS = 0,	/* eight colour bars over a grey ramp, white frame */
	RT_DISPLAY_PATTERN_WHITE = 1,
	RT_DISPLAY_PATTERN_GRADIENT = 2,	/* red across, green down, blue opposite red */
	RT_DISPLAY_PATTERNS
};

/* One DRM connector (writeback connectors are not listed). */
struct rt_display_connector {
	char name[RT_DISPLAY_NAME_BYTES];	/* connector->name, e.g. "DP-1" */
	uint32_t id;			/* DRM object id */
	uint32_t status;		/* enum drm_connector_status: 1 connected, 2 disconnected, 3 unknown */
	uint32_t modes;			/* probed modes (connector->modes) */
	uint32_t edid_bytes;		/* the EDID property blob, 0 without one */
	uint32_t preferred_width, preferred_height, preferred_refresh;	/* Hz */
	uint32_t lit;			/* scanning out the test pattern */
	uint32_t lit_width, lit_height, lit_refresh;
	uint32_t crtc;			/* index of the CRTC that drives it when lit */
};

struct rt_display_report {
	uint32_t version;		/* RT_DISPLAY_VERSION */
	uint32_t connectors;		/* entries of connector[] filled */
	uint32_t showing;		/* the test pattern is on screen */
	uint32_t pattern;		/* enum rt_display_pattern while showing */
	uint32_t fb_width, fb_height, fb_pitch;
	uint32_t crtcs;			/* CRTCs the device has */
	uint64_t fb_gpu_addr;		/* the framebuffer's VRAM address while showing */
	uint64_t fill_ns;		/* writing the pattern */
	uint64_t commit_ns;		/* the last commit (pattern on, or restore) */
	int32_t probe_status;		/* drm_client_modeset_probe() */
	int32_t commit_status;		/* drm_client_modeset_commit() */
	int32_t restore_status;		/* commit of the recorded configuration */
	uint32_t reserved;
	struct rt_display_connector connector[RT_DISPLAY_CONNECTORS_MAX];
};

/* Probe every connector (detect, EDID, modes) with a transient client and
 * report them; nothing is committed. Returns 0 or a negative errno. */
int rt_display_probe(struct pci_dev *pdev, struct rt_display_report *report);

/* Show @pattern on the connected outputs, or only on the connector named
 * @connector ("DP-1" or "card0-DP-1") when it is not NULL or empty, each
 * at its preferred mode. A pattern already showing is turned off first.
 * Returns 0, -ENODEV (no display), -ENOENT (no connected output, or no
 * such connector), -EINVAL, or the failing upstream call's errno; on any
 * failure the recorded configuration is committed again and nothing stays
 * allocated. */
int rt_display_show(struct pci_dev *pdev, const char *connector, uint32_t pattern,
		    struct rt_display_report *report);

/* Turn the pattern off: commit the configuration recorded before it was
 * shown (restore_status reports that commit), delete
 * the framebuffer, release the client. 0 when nothing was showing. */
int rt_display_off(struct pci_dev *pdev, struct rt_display_report *report);

/* rt_display_off() for the session close, before upstream removal. */
void rt_display_stop(void);

/* Whether a pattern is showing (cached; takes no lock). */
int rt_display_showing(void);

#ifdef __cplusplus
}
#endif
#endif
