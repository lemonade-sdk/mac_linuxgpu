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
 * A device that left the bus (rt/removal.h) has no display: every call
 * returns -ENODEV, and turning a pattern or output off commits nothing,
 * releasing the buffers and the client as a Linux unplug does.
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
	uint32_t hotplug_epoch;		/* hotplug events seen (STATUS; 0 before the first) */
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

/* Cached connector state for a display agent: no detection, no AUX or DDC
 * traffic, only what the last probe or hotplug left in each connector,
 * plus report->hotplug_epoch, which counts hotplug events. The first call
 * registers a monitor client (drm_client_register) whose hotplug callback
 * counts them: DM's HPD interrupt handling (drm_kms_helper_connector_
 * hotplug_event) reaches it. Upstream unregisters and frees it with the
 * DRM device. A changed epoch means: probe again. */
int rt_display_status(struct pci_dev *pdev, struct rt_display_report *report);

#define RT_DISPLAY_MODES_MAX	56u
#define RT_DISPLAY_MODE_PREFERRED	(1u << 0)
#define RT_DISPLAY_MODE_INTERLACE	(1u << 1)

struct rt_display_mode {
	uint16_t width, height;
	uint32_t refresh_mhz;		/* vertical refresh in mHz */
	uint32_t clock_khz;
	uint32_t flags;			/* RT_DISPLAY_MODE_* */
};

/* One connector's probed modes (connector->modes after the last probe), in
 * the order upstream sorted them (preferred first). */
struct rt_display_modes {
	uint32_t version;		/* RT_DISPLAY_VERSION */
	uint32_t status;		/* enum drm_connector_status */
	uint32_t count;			/* entries of mode[] filled */
	uint32_t total;			/* modes the connector has */
	uint32_t width_mm, height_mm;	/* display_info physical size */
	struct rt_display_mode mode[RT_DISPLAY_MODES_MAX];
};

/* The modes of the connector named @connector. -ENOENT for no such
 * connector, -ENODEV without display. */
int rt_display_modes(struct pci_dev *pdev, const char *connector, struct rt_display_modes *out);

/* ---- an output a display agent feeds (docs/macos-displays.md) ----
 * rt_display_output() lights one connector at the mode @width x @height at
 * @refresh_mhz (one of its probed modes, matched within 50 mHz; -EINVAL
 * when it has no such mode) with two VRAM framebuffers, recording the
 * configuration before it as the test pattern does; rt_display_off() and
 * rt_display_stop() restore it. While it runs, report->pattern is
 * RT_DISPLAY_PATTERN_OUTPUT.
 *
 * rt_display_present() copies the dirty rectangles of an imported surface
 * (rt/surface.h, the output's size) into the framebuffer not on screen,
 * with SDMA, together with the rectangles of the frame before (that
 * buffer is one frame behind), then flips to it with an atomic commit
 * (drm_client_modeset_commit: a plane update that completes on vblank).
 * No rectangle means nothing changed: nothing is copied or flipped. */
#define RT_DISPLAY_PATTERN_OUTPUT	0xffu
#define RT_DISPLAY_PRESENT_RECTS_MAX	64u

struct rt_surface;
struct rt_surface_rect;

struct rt_display_present_stats {
	uint32_t version;		/* 1 */
	uint32_t rects;			/* rectangles copied */
	uint32_t jobs;			/* SDMA jobs */
	uint32_t full;			/* the whole frame was copied */
	uint64_t bytes;
	uint64_t copy_ns, flip_ns;
	int32_t copy_status, flip_status;
	uint64_t frames;		/* frames presented since the output started */
};

int rt_display_output(struct pci_dev *pdev, const char *connector, uint32_t width,
		      uint32_t height, uint32_t refresh_mhz, struct rt_display_report *report);
int rt_display_present(struct pci_dev *pdev, struct rt_surface *surface,
		       const struct rt_surface_rect *rects, uint32_t count,
		       struct rt_display_present_stats *stats);

/* Whether a pattern is showing (cached; takes no lock). */
int rt_display_showing(void);

#ifdef __cplusplus
}
#endif
#endif
