/* The in-driver display test (rt/display.h): a DRM client that shows a
 * static pattern through upstream drm_client, the atomic helpers and the
 * driver's own KMS (amdgpu_dm and Display Core on amdgpu). */
#include <pthread.h>
#include <time.h>

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <drm/drm_file.h>
#include <drm/drm_vblank.h>
#include <linux/dma-fence.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <drm/drm_auth.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_plane.h>
#include <rt/display.h>
#include <rt/lx_abi.h>
#include <rt/device_string.h>
#include <rt/surface.h>
#include <rt/removal.h>

#include "amdgpu.h"
#include "lx_internal.h"

/* The largest framebuffer the test creates: two 4K monitors side by side
 * would not fit, one 8K mode does. */
#define RT_DISPLAY_FB_MAX 8192u

/* scripts/display-test.py decodes this layout. */
_Static_assert(sizeof(struct rt_display_connector) == 80, "rt_display_connector layout");
_Static_assert(sizeof(struct rt_display_report) == 72 + 8 * 80, "rt_display_report layout");
_Static_assert(sizeof(struct rt_display_mode) == 16 && sizeof(struct rt_display_modes) == 24 + 56 * 16,
	       "rt_display_modes layout");

/* host/DisplayAgent.swift decodes these. */
_Static_assert(sizeof(struct rt_display_present_stats) == 120, "rt_display_present_stats layout");
_Static_assert(sizeof(struct rt_surface_verify_result) == 64, "rt_surface_verify_result layout");

static DEFINE_MUTEX(rt_display_lock);

/* The display configuration before the pattern: per CRTC its mode and
 * enable/active, per plane its CRTC, framebuffer (referenced) and
 * rectangles, per connector its CRTC (connector referenced). */
struct display_snapshot {
	unsigned int crtcs, planes, connectors;
	struct snapshot_crtc {
		struct drm_crtc *crtc;
		bool enable, active;
		struct drm_display_mode mode;
	} *crtc;
	struct snapshot_plane {
		struct drm_plane *plane;
		struct drm_crtc *crtc;
		struct drm_framebuffer *fb;
		int32_t crtc_x, crtc_y;
		uint32_t crtc_w, crtc_h, src_x, src_y, src_w, src_h;
		unsigned int rotation;
	} *plane;
	struct snapshot_connector {
		struct drm_connector *connector;
		struct drm_crtc *crtc;
	} *connector;
};

#define OUTPUT_BUFFERS		3
#define OUTPUT_DAMAGE_MAX	128u
#define OUTPUT_FLIP_TIMEOUT_MS	200u
#define OUTPUT_COPY_TIMEOUT_MS	2000u

extern struct dma_fence *drm_crtc_create_fence(struct drm_crtc *crtc);

struct output_damage {
	struct rt_surface_rect rect[OUTPUT_DAMAGE_MAX];
	uint32_t count;
	bool full;
};

struct output_frame {
	uint64_t capture_ns, received_ns, submitted_ns, bytes;
	struct dma_fence *copy[RT_SURFACE_ENGINES_MAX];
};

/* A client's framebuffer for one commit (LX_SCANOUT PRESENT): the layer
 * (MLG_LX_LAYER_*), the framebuffer and the syncobj that gets the flip's
 * fence, both referenced; src in framebuffer pixels, dst in CRTC pixels. */
struct output_layer {
	uint32_t layer;
	struct drm_framebuffer *fb;
	struct drm_syncobj *sync;
	uint32_t src_x, src_y, src_w, src_h;
	int32_t dst_x, dst_y;
	uint32_t dst_w, dst_h;
};

/* The client attached to the output (rt_display_lx_hooks). The owner and
 * the overlay plane change under rt_display_lock; the mailbox, the detach
 * request, primary_owned and the counters under the output's lock; shown
 * is the worker's. */
struct output_client {
	void *owner;
	struct drm_plane *overlay;	/* reserved for the client, or NULL */
	bool has_next;
	struct output_layer next;
	bool detach;
	struct completion detached;
	uint32_t shown;			/* the layer the screen shows now, 0: none */
	bool primary_owned;		/* shown == MLG_LX_LAYER_PRIMARY, for presents */
	uint32_t last_layer;
	uint64_t presents, flips, desktop_held;
};

struct display_output {
	struct drm_device *dev;
	struct amdgpu_device *adev;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	struct drm_connector *connector;	/* the modeset's, referenced by the client */
	uint32_t width, height, refresh_mhz;
	struct drm_client_buffer *fb[OUTPUT_BUFFERS];
	uint64_t fb_address[OUTPUT_BUFFERS];
	bool pinned[OUTPUT_BUFFERS];
	struct output_damage missed[OUTPUT_BUFFERS];
	struct rt_surface_engines engines;
	/* The worker's own state. */
	int front, pending;
	struct dma_fence *pending_flip;
	struct dma_fence_cb flip_cb;
	struct output_frame pending_frame;
	bool pending_desktop, pending_client;	/* what the pending flip shows */
	struct task_struct *worker;
	/* The worker sleeps on this until a kick (a frame, a flip, stop) or
	 * its timeout; nothing polls. A plain condition variable: linuxu's
	 * wait_event sleeps in 1 ms steps. */
	pthread_mutex_t sleep_lock;
	pthread_cond_t sleep_cond;
	uint64_t kicks, kicks_seen;	/* kicks_seen: the worker's */
	bool stop;
	/* Under lock: the mailbox, the stats, the flip's signal. */
	spinlock_t lock;
	struct rt_surface *next;
	struct output_damage next_damage;
	uint64_t next_capture_ns, next_received_ns;
	bool flip_done;
	struct rt_display_present_stats stats;
	struct output_client client;
};

static void output_stop(struct display_output *o);
static void output_free(struct display_output *o);

/* Guarded by rt_display_lock. */
static struct {
	struct drm_device *dev;
	struct drm_client_dev client;
	struct drm_client_buffer *buffer;
	struct display_snapshot *saved;	/* the configuration before the pattern */
	uint32_t pattern;
	uint64_t fill_ns, commit_ns;
	int restore_status;
	/* An output a display agent feeds (rt_display_output). */
	struct display_output *output;
} rt_display;
static int rt_display_on;	/* atomic copy of "a pattern is showing" */

static struct drm_device *display_device(struct pci_dev *pdev)
{
	struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;

	/* No display IP block (amdgpu.dc=0, or no DCE/DCN): no CRTC was ever
	 * created and mode_config holds only zeroes. */
	if (!dev || !drm_core_check_feature(dev, DRIVER_MODESET) ||
	    !dev->mode_config.num_crtc || !dev->mode_config.num_connector)
		return NULL;
	/* A device that left the bus has no display to drive (rt/removal.h). */
	if (rt_removal_active(drm_to_adev(dev)))
		return NULL;
	return dev;
}

static void name_copy(char out[RT_DISPLAY_NAME_BYTES], const char *name)
{
	strscpy(out, name ? name : "", RT_DISPLAY_NAME_BYTES);
}

/* drm_client_modeset_release(), which is static upstream: drop one CRTC's
 * configuration (its mode copy and connector references). */
static void modeset_clear(struct drm_device *dev, struct drm_mode_set *modeset)
{
	unsigned int i;

	drm_mode_destroy(dev, modeset->mode);
	modeset->mode = NULL;
	modeset->fb = NULL;
	for (i = 0; i < modeset->num_connectors; i++) {
		drm_connector_put(modeset->connectors[i]);
		modeset->connectors[i] = NULL;
	}
	modeset->num_connectors = 0;
}

static bool connector_named(const struct drm_connector *connector, const char *name)
{
	char card[RT_DISPLAY_NAME_BYTES + 16];

	if (!strcmp(connector->name, name))
		return true;
	snprintf(card, sizeof(card), "card%d-%s", connector->dev->primary ?
		 connector->dev->primary->index : 0, connector->name);
	return !strcmp(card, name);
}

/* Keep only the CRTC configuration that drives @name, with that connector
 * alone. Returns the CRTCs left lit. */
static unsigned int modesets_select(struct drm_client_dev *client, const char *name)
{
	struct drm_mode_set *modeset;
	unsigned int lit = 0;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(modeset, client) {
		unsigned int i, keep = modeset->num_connectors;

		for (i = 0; name && name[0] && i < modeset->num_connectors; i++)
			if (connector_named(modeset->connectors[i], name))
				keep = i;
		if (name && name[0] && keep == modeset->num_connectors) {
			modeset_clear(client->dev, modeset);
			continue;
		}
		if (name && name[0]) {
			struct drm_connector *chosen = modeset->connectors[keep];

			for (i = 0; i < modeset->num_connectors; i++)
				if (i != keep)
					drm_connector_put(modeset->connectors[i]);
			modeset->connectors[0] = chosen;
			for (i = 1; i < modeset->num_connectors; i++)
				modeset->connectors[i] = NULL;
			modeset->num_connectors = 1;
		}
		if (modeset->mode && modeset->num_connectors)
			lit++;
		else
			modeset_clear(client->dev, modeset);
	}
	mutex_unlock(&client->modeset_mutex);
	return lit;
}

/* The framebuffer every lit CRTC scans out of: wide and tall enough for
 * each CRTC's mode at its offset. */
static int modesets_extent(struct drm_client_dev *client, u32 *width, u32 *height)
{
	struct drm_mode_set *modeset;

	*width = *height = 0;
	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(modeset, client) {
		if (!modeset->mode)
			continue;
		*width = max_t(u32, *width, modeset->x + modeset->mode->hdisplay);
		*height = max_t(u32, *height, modeset->y + modeset->mode->vdisplay);
	}
	mutex_unlock(&client->modeset_mutex);
	if (!*width || !*height)
		return -ENOENT;
	if (*width > RT_DISPLAY_FB_MAX || *height > RT_DISPLAY_FB_MAX)
		return -E2BIG;
	return 0;
}

static void modesets_attach(struct drm_client_dev *client, struct drm_framebuffer *fb)
{
	struct drm_mode_set *modeset;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(modeset, client)
		if (modeset->mode)
			modeset->fb = fb;
	mutex_unlock(&client->modeset_mutex);
}

/* One row of the pattern, XRGB8888. */
static void pattern_row(u32 *row, u32 width, u32 y, u32 height, uint32_t pattern)
{
	static const u32 bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
				     0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	u32 x;

	for (x = 0; x < width; x++) {
		u32 pixel;

		switch (pattern) {
		case RT_DISPLAY_PATTERN_WHITE:
			pixel = 0xffffff;
			break;
		case RT_DISPLAY_PATTERN_GRADIENT: {
			u32 r = width > 1 ? x * 255 / (width - 1) : 0;
			u32 g = height > 1 ? y * 255 / (height - 1) : 0;

			pixel = r << 16 | g << 8 | (255 - r);
			break;
		}
		default:
			if (y < 4 || y + 4 >= height || x < 4 || x + 4 >= width) {
				pixel = 0xffffff;	/* frame: shows cropping or overscan */
			} else if (y < height * 3 / 4) {
				pixel = bars[(u64)x * 8 / width];
			} else {
				u32 v = width > 1 ? x * 255 / (width - 1) : 0;

				pixel = v << 16 | v << 8 | v;
			}
			break;
		}
		row[x] = pixel;
	}
}

/* Copy one row into the buffer. CPU-visible VRAM is I/O memory: in the
 * dext it is the BAR0 CPU mapping, written with naturally aligned 64-bit
 * stores (writeq), never with unaligned or zeroing accesses, and much
 * faster than memcpy_toio's byte stores over Thunderbolt. Host builds
 * route readX/writeX to synthetic MMIO tokens, so there the fixture's
 * VRAM (host memory) takes memcpy_toio, which is a plain copy. */
static void fb_write(struct iosys_map *map, size_t offset, const u32 *row, size_t bytes)
{
	if (!map->is_iomem) {
		memcpy(map->vaddr + offset, row, bytes);
		return;
	}
#ifdef LINUXU_DEXT_DK
	{
		u8 __iomem *dst = map->vaddr_iomem + offset;
		size_t at = 0;

		if (!((uintptr_t)dst & 7))
			for (; at + 8 <= bytes; at += 8)
				writeq(*(const u64 *)((const u8 *)row + at), dst + at);
		for (; at + 4 <= bytes; at += 4)
			writel(*(const u32 *)((const u8 *)row + at), dst + at);
	}
#else
	memcpy_toio(map->vaddr_iomem + offset, row, bytes);
#endif
}

static int pattern_fill(struct drm_client_buffer *buffer, uint32_t pattern, uint64_t *ns)
{
	struct drm_framebuffer *fb = buffer->fb;
	struct iosys_map map;
	u64 start = ktime_get_ns();
	u32 *row;
	u32 y;
	int ret;

	row = kmalloc_array(fb->width, sizeof(*row), GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	ret = drm_client_buffer_vmap(buffer, &map);
	if (ret) {
		kfree(row);
		return ret;
	}
	for (y = 0; y < fb->height; y++) {
		pattern_row(row, fb->width, y, fb->height, pattern);
		fb_write(&map, (size_t)y * fb->pitches[0], row, (size_t)fb->width * sizeof(*row));
	}
	drm_client_buffer_vunmap(buffer);
	kfree(row);
	/* No drm_client_buffer_flush(): the pattern is complete before the
	 * first commit scans it out (amdgpu's dirtyfb refuses client files
	 * with -ENOSYS, as DIRTYFB callers expect when no flush is needed). */
	*ns = ktime_get_ns() - start;
	return 0;
}

static void snapshot_free(struct display_snapshot *snap)
{
	unsigned int i;

	if (!snap)
		return;
	for (i = 0; snap->plane && i < snap->planes; i++)
		if (snap->plane[i].fb)
			drm_framebuffer_put(snap->plane[i].fb);
	for (i = 0; snap->connector && i < snap->connectors; i++)
		drm_connector_put(snap->connector[i].connector);
	kfree(snap->crtc);
	kfree(snap->plane);
	kfree(snap->connector);
	kfree(snap);
}

/* Record the current configuration (the committed states, read under all
 * modeset locks). Not a duplicated drm_atomic_state: a driver's private
 * objects (amdgpu_dm's DC context) must not be committed back stale;
 * restoring builds a fresh state from the current one instead. */
static int state_save(struct drm_device *dev, struct display_snapshot **out)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_list_iter iter;
	struct display_snapshot *snap;
	struct drm_connector *connector;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	int ret;

	*out = NULL;
	snap = kzalloc(sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	snap->crtc = kcalloc(dev->mode_config.num_crtc, sizeof(*snap->crtc), GFP_KERNEL);
	snap->plane = kcalloc(dev->mode_config.num_total_plane, sizeof(*snap->plane), GFP_KERNEL);
	snap->connector = kcalloc(dev->mode_config.num_connector, sizeof(*snap->connector), GFP_KERNEL);
	if (!snap->crtc || !snap->plane || !snap->connector) {
		snapshot_free(snap);
		return -ENOMEM;
	}

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);
	drm_for_each_crtc(crtc, dev) {
		struct snapshot_crtc *c = &snap->crtc[snap->crtcs++];

		c->crtc = crtc;
		c->enable = crtc->state->enable;
		c->active = crtc->state->active;
		if (c->enable)
			drm_mode_copy(&c->mode, &crtc->state->mode);
	}
	drm_for_each_plane(plane, dev) {
		struct snapshot_plane *p = &snap->plane[snap->planes++];
		const struct drm_plane_state *ps = plane->state;

		p->plane = plane;
		p->crtc = ps->crtc;
		p->fb = ps->fb;
		if (p->fb)
			drm_framebuffer_get(p->fb);
		p->crtc_x = ps->crtc_x;
		p->crtc_y = ps->crtc_y;
		p->crtc_w = ps->crtc_w;
		p->crtc_h = ps->crtc_h;
		p->src_x = ps->src_x;
		p->src_y = ps->src_y;
		p->src_w = ps->src_w;
		p->src_h = ps->src_h;
		p->rotation = ps->rotation;
	}
	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct snapshot_connector *c;

		if (snap->connectors == dev->mode_config.num_connector)
			break;
		c = &snap->connector[snap->connectors++];
		drm_connector_get(connector);
		c->connector = connector;
		c->crtc = connector->state ? connector->state->crtc : NULL;
	}
	drm_connector_list_iter_end(&iter);
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
	if (ret) {
		snapshot_free(snap);
		return ret;
	}
	*out = snap;
	return 0;
}

/* Commit the recorded configuration as one new atomic state, the way
 * drm_client_modeset_commit_atomic() builds one: every plane, connector
 * and CRTC gets its recorded CRTC, framebuffer, rectangles, mode and
 * active flag; the driver checks and commits it as any other request. */
static int state_apply(struct drm_device *dev, const struct display_snapshot *snap,
		       struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_atomic_state *state;
	unsigned int i;
	int ret = 0;

	state = drm_atomic_state_alloc(dev);
	if (!state)
		return -ENOMEM;
	state->acquire_ctx = ctx;
	for (i = 0; !ret && i < snap->planes; i++) {
		const struct snapshot_plane *p = &snap->plane[i];
		struct drm_plane_state *ps = drm_atomic_get_plane_state(state, p->plane);

		if (IS_ERR(ps)) {
			ret = PTR_ERR(ps);
			break;
		}
		ret = drm_atomic_set_crtc_for_plane(ps, p->crtc);
		if (ret)
			break;
		drm_atomic_set_fb_for_plane(ps, p->fb);
		ps->crtc_x = p->crtc_x;
		ps->crtc_y = p->crtc_y;
		ps->crtc_w = p->crtc_w;
		ps->crtc_h = p->crtc_h;
		ps->src_x = p->src_x;
		ps->src_y = p->src_y;
		ps->src_w = p->src_w;
		ps->src_h = p->src_h;
		ps->rotation = p->rotation;
	}
	for (i = 0; !ret && i < snap->connectors; i++) {
		const struct snapshot_connector *c = &snap->connector[i];
		struct drm_connector_state *cs = drm_atomic_get_connector_state(state, c->connector);

		ret = IS_ERR(cs) ? PTR_ERR(cs) : drm_atomic_set_crtc_for_connector(cs, c->crtc);
	}
	for (i = 0; !ret && i < snap->crtcs; i++) {
		const struct snapshot_crtc *c = &snap->crtc[i];
		struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, c->crtc);

		if (IS_ERR(cs)) {
			ret = PTR_ERR(cs);
			break;
		}
		ret = drm_atomic_set_mode_for_crtc(cs, c->enable ? &c->mode : NULL);
		cs->active = c->active;
	}
	if (!ret)
		ret = drm_atomic_commit(state);
	drm_atomic_state_put(state);
	return ret;
}

static int state_restore(struct drm_device *dev, const struct display_snapshot *snap)
{
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);
	ret = state_apply(dev, snap, &ctx);
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
	return ret;
}

/* Undo everything show built, in reverse: the saved configuration goes
 * back on screen before the framebuffer it replaced is removed, so the removal
 * never has to disable a plane itself. */
static int display_off_locked(void)
{
	struct drm_device *dev = rt_display.dev;
	int ret = 0;

	if (!dev)
		return 0;
	/* An output's worker stops first: no flip of its may follow. */
	if (rt_display.output)
		output_stop(rt_display.output);
	if (rt_display.saved && rt_removal_active(drm_to_adev(dev))) {
		/* The device left the bus: there is no screen to restore. The
		 * buffers and the client go as on a Linux unplug, where the
		 * removed framebuffers' planes are disabled without hardware. */
		drm_info(dev, "display test: device removed; the previous state is not committed\n");
		snapshot_free(rt_display.saved);
		rt_display.saved = NULL;
	}
	if (rt_display.saved) {
		u64 start = ktime_get_ns();

		ret = state_restore(dev, rt_display.saved);
		snapshot_free(rt_display.saved);
		rt_display.saved = NULL;
		rt_display.commit_ns = ktime_get_ns() - start;
		rt_display.restore_status = ret;
		if (ret)
			drm_err(dev, "display test: restoring the previous state failed (%d)\n", ret);
		else
			drm_info(dev, "display test: previous display state restored (%llu ms)\n",
				 rt_display.commit_ns / 1000000);
	}
	if (rt_display.buffer) {
		drm_client_buffer_delete(rt_display.buffer);
		rt_display.buffer = NULL;
	}
	if (rt_display.output) {
		output_free(rt_display.output);
		rt_display.output = NULL;
	}
	drm_client_release(&rt_display.client);
	memset(&rt_display.client, 0, sizeof(rt_display.client));
	rt_display.dev = NULL;
	__atomic_store_n(&rt_display_on, 0, __ATOMIC_RELEASE);
	return ret;
}

static void report_connectors(struct drm_device *dev, struct drm_client_dev *lit,
			      struct rt_display_report *report)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	report->crtcs = dev->mode_config.num_crtc;
	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &iter);
	drm_client_for_each_connector_iter(connector, &iter) {
		struct rt_display_connector *out;
		struct drm_display_mode *mode, *preferred = NULL;

		if (report->connectors == RT_DISPLAY_CONNECTORS_MAX)
			break;
		out = &report->connector[report->connectors++];
		name_copy(out->name, connector->name);
		out->id = connector->base.id;
		out->status = connector->status;
		out->edid_bytes = connector->edid_blob_ptr ? connector->edid_blob_ptr->length : 0;
		list_for_each_entry(mode, &connector->modes, head) {
			out->modes++;
			if (!preferred || ((mode->type & DRM_MODE_TYPE_PREFERRED) &&
					   !(preferred->type & DRM_MODE_TYPE_PREFERRED)))
				preferred = mode;
		}
		if (preferred) {
			out->preferred_width = preferred->hdisplay;
			out->preferred_height = preferred->vdisplay;
			out->preferred_refresh = drm_mode_vrefresh(preferred);
		}
		if (lit) {
			struct drm_mode_set *modeset;

			mutex_lock(&lit->modeset_mutex);
			drm_client_for_each_modeset(modeset, lit) {
				if (!modeset->mode || !modeset->fb || !modeset->num_connectors ||
				    modeset->connectors[0] != connector)
					continue;
				out->lit = 1;
				out->lit_width = modeset->mode->hdisplay;
				out->lit_height = modeset->mode->vdisplay;
				out->lit_refresh = drm_mode_vrefresh(modeset->mode);
				out->crtc = drm_crtc_index(modeset->crtc);
			}
			mutex_unlock(&lit->modeset_mutex);
		}
	}
	drm_connector_list_iter_end(&iter);
	mutex_unlock(&dev->mode_config.mutex);
}

static void report_state(struct drm_device *dev, struct rt_display_report *report)
{
	if (!report || !dev)
		return;
	report_connectors(dev, rt_display.dev == dev && rt_display.saved ? &rt_display.client : NULL,
			  report);
	report->showing = rt_display.dev == dev && rt_display.saved;
	report->restore_status = rt_display.restore_status;
	report->commit_ns = rt_display.commit_ns;
	if (report->showing)
		report->pattern = rt_display.pattern;
	if (report->showing && rt_display.output) {
		struct display_output *o = rt_display.output;
		/* The buffer the output started on (the worker owns which is
		 * on screen now; their geometry is the same). */
		struct drm_framebuffer *fb = o->fb[0]->fb;

		report->fb_width = fb->width;
		report->fb_height = fb->height;
		report->fb_pitch = fb->pitches[0];
		report->fb_gpu_addr = o->fb_address[0];
	} else if (report->showing && rt_display.buffer) {
		struct drm_framebuffer *fb = rt_display.buffer->fb;
		struct amdgpu_bo *bo = gem_to_amdgpu_bo(fb->obj[0]);

		report->fb_width = fb->width;
		report->fb_height = fb->height;
		report->fb_pitch = fb->pitches[0];
		report->fill_ns = rt_display.fill_ns;
		/* Pinned for scanout by the plane's prepare_fb. */
		if (bo->tbo.pin_count)
			report->fb_gpu_addr = amdgpu_bo_gpu_offset(bo);
	}
}

static void report_begin(struct rt_display_report *report)
{
	if (!report)
		return;
	memset(report, 0, sizeof(*report));
	report->version = RT_DISPLAY_VERSION;
}

int rt_display_probe(struct pci_dev *pdev, struct rt_display_report *report)
{
	struct drm_device *dev = display_device(pdev);
	struct drm_client_dev *client;
	int ret;

	report_begin(report);
	if (!dev)
		return -ENODEV;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	ret = drm_client_init(dev, client, "linuxu-display-probe", NULL);
	if (ret) {
		kfree(client);
		return ret;
	}
	/* Runs each connector's fill_modes(): detect, EDID, mode list. */
	ret = drm_client_modeset_probe(client, 0, 0);
	if (report)
		report->probe_status = ret;
	drm_client_release(client);
	kfree(client);
	mutex_lock(&rt_display_lock);
	report_state(dev, report);
	mutex_unlock(&rt_display_lock);
	return ret;
}

int rt_display_show(struct pci_dev *pdev, const char *connector, uint32_t pattern,
		    struct rt_display_report *report)
{
	struct drm_device *dev = display_device(pdev);
	struct drm_client_buffer *buffer;
	unsigned int lit;
	u32 width, height;
	u64 start;
	int ret;

	report_begin(report);
	if (!dev)
		return -ENODEV;
	if (pattern >= RT_DISPLAY_PATTERNS ||
	    (connector && strnlen(connector, RT_DISPLAY_NAME_BYTES) >= RT_DISPLAY_NAME_BYTES))
		return -EINVAL;

	mutex_lock(&rt_display_lock);
	if (rt_display.dev)
		(void)display_off_locked();
	rt_display.restore_status = 0;
	rt_display.commit_ns = rt_display.fill_ns = 0;
	ret = drm_client_init(dev, &rt_display.client, "linuxu-display-test", NULL);
	if (ret) {
		memset(&rt_display.client, 0, sizeof(rt_display.client));
		goto out;
	}
	rt_display.dev = dev;
	rt_display.pattern = pattern;

	ret = drm_client_modeset_probe(&rt_display.client, 0, 0);
	if (report)
		report->probe_status = ret;
	if (ret)
		goto fail;
	lit = modesets_select(&rt_display.client, connector);
	if (!lit) {
		drm_info(dev, "display test: no connected output%s%s\n",
			 connector && connector[0] ? " named " : "",
			 connector && connector[0] ? connector : "");
		ret = -ENOENT;
		goto fail;
	}
	ret = modesets_extent(&rt_display.client, &width, &height);
	if (ret)
		goto fail;

	buffer = drm_client_buffer_create_dumb(&rt_display.client, width, height,
					       DRM_FORMAT_XRGB8888);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		drm_err(dev, "display test: %ux%u framebuffer failed (%d)\n", width, height, ret);
		goto fail;
	}
	rt_display.buffer = buffer;
	ret = pattern_fill(buffer, pattern, &rt_display.fill_ns);
	if (ret) {
		drm_err(dev, "display test: writing the pattern failed (%d)\n", ret);
		goto fail;
	}
	modesets_attach(&rt_display.client, buffer->fb);

	ret = state_save(dev, &rt_display.saved);
	if (ret) {
		drm_err(dev, "display test: saving the current state failed (%d)\n", ret);
		goto fail;
	}
	drm_info(dev, "display test: committing a %ux%u framebuffer on %u CRTC(s), pattern %u\n",
		 width, height, lit, pattern);
	start = ktime_get_ns();
	ret = drm_client_modeset_commit(&rt_display.client);
	rt_display.commit_ns = ktime_get_ns() - start;
	if (report)
		report->commit_status = ret;
	if (ret) {
		drm_err(dev, "display test: commit failed (%d)\n", ret);
		goto fail;
	}
	__atomic_store_n(&rt_display_on, 1, __ATOMIC_RELEASE);
	drm_info(dev, "display test: pattern on screen (fill %llu ms, commit %llu ms)\n",
		 rt_display.fill_ns / 1000000, rt_display.commit_ns / 1000000);
	report_state(dev, report);
	mutex_unlock(&rt_display_lock);
	return 0;

fail:
	/* Restores the saved state when one was taken (the commit may have
	 * changed part of the hardware before failing), then frees. */
	(void)display_off_locked();
out:
	if (report) {
		int probe = report->probe_status, commit = report->commit_status;

		report_state(dev, report);
		report->probe_status = probe;
		report->commit_status = commit;
	}
	mutex_unlock(&rt_display_lock);
	return ret;
}

int rt_display_off(struct pci_dev *pdev, struct rt_display_report *report)
{
	struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;
	int ret;

	report_begin(report);
	mutex_lock(&rt_display_lock);
	ret = display_off_locked();
	if (display_device(pdev))
		report_state(dev, report);
	if (report)
		report->restore_status = ret;
	mutex_unlock(&rt_display_lock);
	return ret;
}

void rt_display_stop(void)
{
	mutex_lock(&rt_display_lock);
	(void)display_off_locked();
	rt_display.restore_status = 0;
	mutex_unlock(&rt_display_lock);
}

/* ---- the monitor client: hotplug events for a display agent ---- */

static struct drm_client_dev *rt_monitor;	/* registered; upstream owns its end */
static uint32_t rt_hotplug_epoch;

static int monitor_hotplug(struct drm_client_dev *client)
{
	(void)client;
	__atomic_add_fetch(&rt_hotplug_epoch, 1, __ATOMIC_RELEASE);
	return 0;
}

/* drm_client_dev_unregister(), with dev->clientlist_mutex held: release
 * and free. Takes no lock of ours (show/probe take rt_display_lock, then
 * clientlist_mutex when registering). */
static void monitor_unregister(struct drm_client_dev *client)
{
	struct drm_client_dev *expected = client;

	__atomic_compare_exchange_n(&rt_monitor, &expected, NULL, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	drm_client_release(client);
}

static void monitor_free(struct drm_client_dev *client)
{
	kfree(client);
}

static const struct drm_client_funcs monitor_funcs = {
	.owner = THIS_MODULE,
	.hotplug = monitor_hotplug,
	.unregister = monitor_unregister,
	.free = monitor_free,
};

/* Caller holds rt_display_lock. */
static int monitor_ensure(struct drm_device *dev)
{
	struct drm_client_dev *client;
	int ret;

	client = __atomic_load_n(&rt_monitor, __ATOMIC_ACQUIRE);
	if (client && client->dev == dev)
		return 0;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	ret = drm_client_init(dev, client, "linuxu-display-monitor", &monitor_funcs);
	if (ret) {
		kfree(client);
		return ret;
	}
	__atomic_store_n(&rt_monitor, client, __ATOMIC_RELEASE);
	/* Generates the first hotplug event (epoch 1). */
	drm_client_register(client);
	return 0;
}

int rt_display_status(struct pci_dev *pdev, struct rt_display_report *report)
{
	struct drm_device *dev = display_device(pdev);
	int ret;

	report_begin(report);
	if (!dev)
		return -ENODEV;
	mutex_lock(&rt_display_lock);
	ret = monitor_ensure(dev);
	report_state(dev, report);
	mutex_unlock(&rt_display_lock);
	if (report)
		report->hotplug_epoch = __atomic_load_n(&rt_hotplug_epoch, __ATOMIC_ACQUIRE);
	return ret;
}

int rt_display_modes(struct pci_dev *pdev, const char *name, struct rt_display_modes *out)
{
	struct drm_device *dev = display_device(pdev);
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	int ret = -ENOENT;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->version = RT_DISPLAY_VERSION;
	if (!dev)
		return -ENODEV;
	if (!name || !name[0])
		return -EINVAL;
	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &iter);
	drm_client_for_each_connector_iter(connector, &iter) {
		struct drm_display_mode *mode;

		if (!connector_named(connector, name))
			continue;
		out->status = connector->status;
		out->width_mm = connector->display_info.width_mm;
		out->height_mm = connector->display_info.height_mm;
		list_for_each_entry(mode, &connector->modes, head) {
			struct rt_display_mode *m;
			u64 total = (u64)mode->htotal * mode->vtotal;

			out->total++;
			if (out->count == RT_DISPLAY_MODES_MAX)
				continue;
			m = &out->mode[out->count++];
			m->width = mode->hdisplay;
			m->height = mode->vdisplay;
			m->clock_khz = mode->clock;
			m->refresh_mhz = total ? (u32)div64_u64((u64)mode->clock * 1000000ull, total) : 0;
			if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
				m->refresh_mhz /= 2;
			if (mode->flags & DRM_MODE_FLAG_INTERLACE)
				m->refresh_mhz *= 2;
			m->flags = (mode->type & DRM_MODE_TYPE_PREFERRED ? RT_DISPLAY_MODE_PREFERRED : 0) |
				   (mode->flags & DRM_MODE_FLAG_INTERLACE ? RT_DISPLAY_MODE_INTERLACE : 0);
		}
		ret = 0;
		break;
	}
	drm_connector_list_iter_end(&iter);
	mutex_unlock(&dev->mode_config.mutex);
	return ret;
}

int rt_display_monitor(struct pci_dev *pdev, const char *name, struct rt_display_monitor *out)
{
	struct drm_device *dev = display_device(pdev);
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	int ret = -ENOENT;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!dev)
		return -ENODEV;
	if (!name || !name[0])
		return -EINVAL;
	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &iter);
	drm_client_for_each_connector_iter(connector, &iter) {
		const struct drm_property_blob *edid = connector->edid_blob_ptr;

		if (!connector_named(connector, name))
			continue;
		out->status = connector->status;
		out->width_mm = connector->display_info.width_mm;
		out->height_mm = connector->display_info.height_mm;
		if (edid && edid->length >= EDID_LENGTH)
			drm_edid_get_monitor_name(edid->data, out->name, sizeof(out->name));
		ret = 0;
		break;
	}
	drm_connector_list_iter_end(&iter);
	mutex_unlock(&dev->mode_config.mutex);
	return ret;
}

/* ---- an output a display agent feeds ---- */

static uint32_t mode_refresh_mhz(const struct drm_display_mode *mode)
{
	u64 total = (u64)mode->htotal * mode->vtotal;
	uint32_t mhz = total ? (u32)div64_u64((u64)mode->clock * 1000000ull, total) : 0;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		mhz /= 2;
	return mhz;
}

/* Put the connector's mode @width x @height @refresh_mhz (progressive) on
 * the single lit modeset. */
static int modeset_use_mode(struct drm_client_dev *client, uint32_t width, uint32_t height,
			    uint32_t refresh_mhz)
{
	struct drm_device *dev = client->dev;
	struct drm_mode_set *modeset, *lit = NULL;
	struct drm_display_mode *mode, *found = NULL;
	int ret = -EINVAL;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(modeset, client)
		if (modeset->mode && modeset->num_connectors)
			lit = modeset;
	if (lit) {
		mutex_lock(&dev->mode_config.mutex);
		list_for_each_entry(mode, &lit->connectors[0]->modes, head) {
			uint32_t mhz = mode_refresh_mhz(mode);

			if (mode->hdisplay == width && mode->vdisplay == height &&
			    !(mode->flags & DRM_MODE_FLAG_INTERLACE) &&
			    (mhz > refresh_mhz ? mhz - refresh_mhz : refresh_mhz - mhz) <= 50) {
				found = mode;
				break;
			}
		}
		if (found) {
			struct drm_display_mode *copy = drm_mode_duplicate(dev, found);

			if (copy) {
				drm_mode_destroy(dev, lit->mode);
				lit->mode = copy;
				lit->x = lit->y = 0;
				ret = 0;
			} else {
				ret = -ENOMEM;
			}
		}
		mutex_unlock(&dev->mode_config.mutex);
	}
	mutex_unlock(&client->modeset_mutex);
	return ret;
}

/* ---- the output pipeline ----
 *
 * PRESENT is a mailbox: it queues the newest frame (its surface, damage and
 * capture time), merging a frame the worker has not taken yet, wakes the
 * worker and returns. The worker copies into a framebuffer that is neither
 * on screen nor waiting for its flip (three of them), with SDMA and no CPU
 * wait, and flips to it with a nonblocking atomic commit: the copy's fence
 * is on the buffer, so the commit waits for it (an implicit in-fence), and
 * the commit's out-fence (a CRTC fence on a kernel flip event) signals at
 * the flip with its vblank timestamp. The worker sleeps on interrupts (the
 * flip's fence callback, a new frame) and never polls; its waits are
 * bounded as a backstop. Each buffer remembers the damage it missed while
 * others were drawn, so a copy is that plus the frame's own. */
static uint64_t rect_area(const struct rt_surface_rect *r)
{
	return (uint64_t)r->width * r->height;
}

/* @a becomes the bounding box of @a and @b when that box is no larger than
 * the two apart: a rectangle inside another, the same rectangle twice, or
 * two that overlap closely. Copying the box then copies no pixel twice and
 * no more pixels than the two would. */
static bool rect_absorb(struct rt_surface_rect *a, const struct rt_surface_rect *b)
{
	const uint64_t ax1 = (uint64_t)a->x + a->width, ay1 = (uint64_t)a->y + a->height;
	const uint64_t bx1 = (uint64_t)b->x + b->width, by1 = (uint64_t)b->y + b->height;
	const uint32_t x0 = min(a->x, b->x), y0 = min(a->y, b->y);
	const uint64_t x1 = max(ax1, bx1), y1 = max(ay1, by1);

	if ((x1 - x0) * (y1 - y0) > rect_area(a) + rect_area(b) || x1 - x0 > UINT32_MAX ||
	    y1 - y0 > UINT32_MAX)
		return false;
	a->x = x0;
	a->y = y0;
	a->width = (uint32_t)(x1 - x0);
	a->height = (uint32_t)(y1 - y0);
	return true;
}

/* Add one rectangle, folding it into any it overlaps closely (repeatedly,
 * as a fold can make a rectangle that absorbs others). The same region
 * damaged in successive frames (a window redrawing in place) stays one
 * rectangle, so a buffer that missed it and the frame that redraws it
 * copy it once, not once per frame. */
static void damage_add_rect(struct output_damage *d, const struct rt_surface_rect *r)
{
	struct rt_surface_rect cur = *r;

	if (d->full || !cur.width || !cur.height)
		return;
	for (bool again = true; again;) {
		again = false;
		for (uint32_t i = 0; i < d->count; i++) {
			struct rt_surface_rect box = d->rect[i];

			if (rect_absorb(&box, &cur)) {
				cur = box;
				d->rect[i] = d->rect[--d->count];
				again = true;
				break;
			}
		}
	}
	if (d->count == OUTPUT_DAMAGE_MAX) {
		d->full = true;
		d->count = 0;
		return;
	}
	d->rect[d->count++] = cur;
}

static void damage_add(struct output_damage *d, const struct rt_surface_rect *rects, uint32_t count)
{
	for (uint32_t i = 0; i < count && !d->full; i++)
		damage_add_rect(d, &rects[i]);
}

/* Most of the frame damaged: one copy of the whole frame. */
static void damage_settle(struct output_damage *d, uint32_t width, uint32_t height)
{
	uint64_t area = 0;

	if (d->full)
		return;
	for (uint32_t i = 0; i < d->count; i++)
		area += rect_area(&d->rect[i]);
	if (area * 10 >= (uint64_t)width * height * 9) {
		d->full = true;
		d->count = 0;
	}
}

static void damage_merge(struct output_damage *d, const struct output_damage *add)
{
	if (add->full) {
		d->full = true;
		d->count = 0;
	} else {
		damage_add(d, add->rect, add->count);
	}
}

static void output_kick(struct display_output *o)
{
	pthread_mutex_lock(&o->sleep_lock);
	o->kicks++;
	pthread_cond_broadcast(&o->sleep_cond);
	pthread_mutex_unlock(&o->sleep_lock);
}

/* The worker: until a kick since the last sleep, stop, or @timeout_ms
 * (0: none). */
static void output_sleep(struct display_output *o, unsigned int timeout_ms)
{
	/* Relative timed wait: DriverKit's arm64 libsystem_pthread exports
	 * pthread_cond_timedwait_relative_np but not pthread_cond_timedwait
	 * (timer.c sleeps the same way). One wait per call: a spurious wakeup
	 * only ends the sleep early, which the worker tolerates. */
	struct timespec rel = {
		.tv_sec = timeout_ms / 1000,
		.tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
	};

	pthread_mutex_lock(&o->sleep_lock);
	while (o->kicks == o->kicks_seen && !o->stop) {
		if (!timeout_ms)
			pthread_cond_wait(&o->sleep_cond, &o->sleep_lock);
		else {
			pthread_cond_timedwait_relative_np(&o->sleep_cond, &o->sleep_lock, &rel);
			break;
		}
	}
	o->kicks_seen = o->kicks;
	pthread_mutex_unlock(&o->sleep_lock);
}

static void output_flip_signaled(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct display_output *o = container_of(cb, struct display_output, flip_cb);
	unsigned long flags;

	(void)fence;
	spin_lock_irqsave(&o->lock, flags);
	o->flip_done = true;
	spin_unlock_irqrestore(&o->lock, flags);
	output_kick(o);
}

static void output_error(struct display_output *o, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&o->lock, flags);
	if (!o->stats.error)
		o->stats.error = error;
	spin_unlock_irqrestore(&o->lock, flags);
	drm_err(o->dev, "display output: worker stopped by error %d\n", error);
}

/* The pending flip happened: account it, its buffers are on screen. */
static void output_flip_account(struct display_output *o)
{
	struct output_frame *f = &o->pending_frame;
	uint64_t flip_ns = ktime_to_ns(o->pending_flip->timestamp);
	uint64_t gpu_ns = 0, latency = 0;
	unsigned long flags;

	for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++) {
		if (!f->copy[i])
			continue;
		if (dma_fence_is_signaled(f->copy[i]) &&
		    test_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &f->copy[i]->flags)) {
			uint64_t done = ktime_to_ns(f->copy[i]->timestamp);

			if (done > f->submitted_ns && done - f->submitted_ns > gpu_ns)
				gpu_ns = done - f->submitted_ns;
		}
		dma_fence_put(f->copy[i]);
		f->copy[i] = NULL;
	}
	if (f->capture_ns && flip_ns > f->capture_ns)
		latency = flip_ns - f->capture_ns;
	spin_lock_irqsave(&o->lock, flags);
	if (o->pending_desktop) {
		o->stats.frames_flipped++;
		o->stats.copy_gpu_ns += gpu_ns;
		o->stats.last_copy_gpu_ns = gpu_ns;
		o->stats.last_bytes = f->bytes;
		o->stats.latency_ns += latency;
		o->stats.last_latency_ns = latency;
		if (latency > o->stats.latency_max_ns)
			o->stats.latency_max_ns = latency;
	}
	if (o->pending_client)
		o->client.flips++;
	o->flip_done = false;
	spin_unlock_irqrestore(&o->lock, flags);
	dma_fence_put(o->pending_flip);
	o->pending_flip = NULL;
	o->front = o->pending;
	o->pending = -1;
	o->pending_desktop = o->pending_client = false;
}

/* Wait (bounded) for the pending flip; 0 when it happened. */
static int output_flip_wait(struct display_output *o)
{
	long left;

	if (!o->pending_flip)
		return 0;
	/* The flip waits for its copies (the implicit in-fence): those first,
	 * under the bound of GPU work, then the flip, under the vblank's. */
	for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++) {
		struct dma_fence *copy = o->pending_frame.copy[i];

		if (!copy)
			continue;
		left = dma_fence_wait_timeout(copy, false, msecs_to_jiffies(OUTPUT_COPY_TIMEOUT_MS));
		if (left <= 0) {
			pr_err("display: the copy for the pending flip did not complete in %u ms (%ld)\n",
			       OUTPUT_COPY_TIMEOUT_MS, left);
			return left < 0 ? (int)left : -ETIME;
		}
	}
	/* A client's frame waits for its rendering the same way (the
	 * framebuffer's implicit fences), which only the GPU work bounds. */
	left = dma_fence_wait_timeout(o->pending_flip, false,
				      msecs_to_jiffies(o->pending_client ? OUTPUT_COPY_TIMEOUT_MS :
						       OUTPUT_FLIP_TIMEOUT_MS));
	if (left <= 0) {
		pr_err("display: the flip did not complete within %u ms of its %s (%ld)\n",
		       o->pending_client ? OUTPUT_COPY_TIMEOUT_MS : OUTPUT_FLIP_TIMEOUT_MS,
		       o->pending_client ? "client frame" : "copy", left);
		return left < 0 ? (int)left : -ETIME;
	}
	output_flip_account(o);
	return 0;
}

/* @ps shows @fb's @src (pixels) at @dst on @crtc; a NULL @fb takes the
 * plane off. */
static int plane_place(struct drm_plane_state *ps, struct drm_crtc *crtc, struct drm_framebuffer *fb,
		       uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h,
		       int32_t dst_x, int32_t dst_y, uint32_t dst_w, uint32_t dst_h)
{
	int ret = drm_atomic_set_crtc_for_plane(ps, fb ? crtc : NULL);

	if (ret)
		return ret;
	drm_atomic_set_fb_for_plane(ps, fb);
	if (!fb) {
		ps->crtc_x = ps->crtc_y = 0;
		ps->crtc_w = ps->crtc_h = 0;
		ps->src_x = ps->src_y = ps->src_w = ps->src_h = 0;
		return 0;
	}
	ps->crtc_x = dst_x;
	ps->crtc_y = dst_y;
	ps->crtc_w = dst_w;
	ps->crtc_h = dst_h;
	ps->src_x = src_x << 16;
	ps->src_y = src_y << 16;
	ps->src_w = src_w << 16;
	ps->src_h = src_h << 16;
	return 0;
}

static int layer_place(struct drm_plane_state *ps, struct drm_crtc *crtc,
		       const struct output_layer *cl)
{
	return plane_place(ps, crtc, cl->fb, cl->src_x, cl->src_y, cl->src_w, cl->src_h,
			   cl->dst_x, cl->dst_y, cl->dst_w, cl->dst_h);
}

/* The planes of one commit in @state: the primary plane shows desktop
 * buffer @b (-1: the last one shown) unless the client has it; the
 * client's frame @cl, if any, goes on its layer; @off takes the client off
 * the screen. The overlay plane is only touched when it changes. */
static int output_scene(struct display_output *o, struct drm_atomic_state *state, int b,
			const struct output_layer *cl, bool off)
{
	const uint32_t was = o->client.shown;
	const uint32_t now = off ? 0 : cl ? cl->layer : was;
	struct drm_plane_state *ps;
	int ret = 0;

	if (b < 0)
		b = o->front >= 0 ? o->front : 0;
	ps = drm_atomic_get_plane_state(state, o->plane);
	if (IS_ERR(ps))
		return PTR_ERR(ps);
	if (now == MLG_LX_LAYER_PRIMARY && cl)
		ret = layer_place(ps, o->crtc, cl);
	else if (now != MLG_LX_LAYER_PRIMARY)
		ret = plane_place(ps, o->crtc, o->fb[b]->fb, 0, 0, o->width, o->height, 0, 0,
				  o->width, o->height);
	/* else the client's last frame stays on the primary plane */
	if (ret || !o->client.overlay)
		return ret;
	if (now == MLG_LX_LAYER_OVERLAY && cl) {
		ps = drm_atomic_get_plane_state(state, o->client.overlay);
		return IS_ERR(ps) ? PTR_ERR(ps) : layer_place(ps, o->crtc, cl);
	}
	if (was == MLG_LX_LAYER_OVERLAY && now != MLG_LX_LAYER_OVERLAY) {
		ps = drm_atomic_get_plane_state(state, o->client.overlay);
		return IS_ERR(ps) ? PTR_ERR(ps) : plane_place(ps, o->crtc, NULL, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	return 0;
}

/* A nonblocking commit of the scene (output_scene), with a CRTC fence that
 * signals at the flip. */
static int output_commit(struct display_output *o, int b, const struct output_layer *cl, bool off,
			 struct dma_fence **out)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct dma_fence *fence = NULL;
	int ret;

	*out = NULL;
	drm_modeset_acquire_init(&ctx, 0);
	state = drm_atomic_state_alloc(o->dev);
	if (!state) {
		ret = -ENOMEM;
		goto fini;
	}
	state->acquire_ctx = &ctx;
retry:
	{
		struct drm_crtc_state *cs;
		struct drm_pending_vblank_event *e;

		ret = output_scene(o, state, b, cl, off);
		if (ret)
			goto backoff;
		cs = drm_atomic_get_crtc_state(state, o->crtc);
		ret = PTR_ERR_OR_ZERO(cs);
		if (ret)
			goto backoff;
		e = kzalloc(sizeof(*e), GFP_KERNEL);
		fence = drm_crtc_create_fence(o->crtc);
		if (!e || !fence) {
			kfree(e);
			if (fence)
				dma_fence_put(fence);
			fence = NULL;
			ret = -ENOMEM;
			goto put;
		}
		e->pipe = drm_crtc_index(o->crtc);
		e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
		e->event.base.length = sizeof(e->event);
		e->event.vbl.crtc_id = o->crtc->base.id;
		e->base.event = &e->event.base;
		e->base.fence = dma_fence_get(fence);	/* signalled and put at the flip */
		cs->event = e;
		ret = drm_atomic_nonblocking_commit(state);
		if (ret) {
			/* As the atomic ioctl's failure path does. */
			cs->event = NULL;
			drm_event_cancel_free(o->dev, &e->base);
			dma_fence_put(fence);
			fence = NULL;
		}
	}
backoff:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
put:
	drm_atomic_state_put(state);
fini:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	*out = fence;
	return ret;
}

/* An atomic check of the scene with the client's frame @cl, nothing
 * committed (LX_SCANOUT TEST). */
static int output_check(struct display_output *o, const struct output_layer *cl)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);
	state = drm_atomic_state_alloc(o->dev);
	if (!state) {
		ret = -ENOMEM;
		goto fini;
	}
	state->acquire_ctx = &ctx;
retry:
	ret = output_scene(o, state, -1, cl, false);
	if (!ret)
		ret = drm_atomic_check_only(state);
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
fini:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

/* A client frame that will not be shown: its syncobj signals at once (the
 * client's wait ends; its next call reports why) and the references go. */
static void layer_drop(struct output_layer *cl)
{
	if (cl->sync) {
		struct dma_fence *stub = dma_fence_get_stub();

		drm_syncobj_replace_fence(cl->sync, stub);
		dma_fence_put(stub);
		drm_syncobj_put(cl->sync);
	}
	if (cl->fb)
		drm_framebuffer_put(cl->fb);
	memset(cl, 0, sizeof(*cl));
}

/* One step: copy a desktop frame (@surface, may be NULL) into a free
 * buffer, wait for the previous flip, then commit the desktop with the
 * client's frame @cl (may be NULL), or with the client taken off (@off). */
static int output_frame(struct display_output *o, struct rt_surface *surface,
			struct output_damage *damage, uint64_t capture_ns, uint64_t received_ns,
			struct output_layer *cl, bool off)
{
	struct rt_surface_rect full = { 0, 0, o->width, o->height };
	struct output_frame frame = { .capture_ns = capture_ns, .received_ns = received_ns };
	struct dma_fence *flip;
	unsigned long flags;
	int b = -1, r;
	u64 start = ktime_get_ns();

	if (surface) {
		struct output_damage copy;
		struct rt_surface_copy_stats cs;

		b = 0;
		while (b == o->front || b == o->pending)
			b++;
		copy = o->missed[b];
		damage_merge(&copy, damage);
		damage_settle(&copy, o->width, o->height);
		r = rt_surface_copy_submit(surface, o->fb[b]->fb->obj[0], o->fb_address[b],
					   o->fb[b]->fb->pitches[0], copy.full ? &full : copy.rect,
					   copy.full ? 1 : copy.count, &o->engines, frame.copy, &cs);
		rt_surface_release(surface);
		if (r)
			return r;
		frame.submitted_ns = ktime_get_ns();
		frame.bytes = cs.bytes;
		memset(&o->missed[b], 0, sizeof(o->missed[b]));
		for (int i = 0; i < OUTPUT_BUFFERS; i++)
			if (i != b)
				damage_merge(&o->missed[i], damage);
		spin_lock_irqsave(&o->lock, flags);
		o->stats.copy_jobs += cs.jobs;
		o->stats.bytes += cs.bytes;
		o->stats.copy_submit_ns += frame.submitted_ns - start;
		o->stats.full_frames += copy.full;
		spin_unlock_irqrestore(&o->lock, flags);
	}

	/* One flip in flight per CRTC: the previous one first. */
	r = output_flip_wait(o);
	if (!r)
		r = output_commit(o, b, cl, off, &flip);
	if (r) {
		for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
			if (frame.copy[i])
				dma_fence_put(frame.copy[i]);
		return r;
	}
	if (cl) {
		/* The client's syncobj signals at this flip; the plane state
		 * holds the framebuffer now. */
		drm_syncobj_replace_fence(cl->sync, flip);
		drm_syncobj_put(cl->sync);
		drm_framebuffer_put(cl->fb);
		cl->sync = NULL;
		cl->fb = NULL;
	}
	o->pending = b >= 0 ? b : o->front;
	o->pending_flip = flip;
	o->pending_frame = frame;
	o->pending_desktop = b >= 0;
	o->pending_client = cl != NULL;
	spin_lock_irqsave(&o->lock, flags);
	if (off)
		o->client.shown = 0;
	else if (cl)
		o->client.shown = cl->layer;
	o->flip_done = false;
	o->client.primary_owned = o->client.shown == MLG_LX_LAYER_PRIMARY;
	spin_unlock_irqrestore(&o->lock, flags);
	if (dma_fence_add_callback(flip, &o->flip_cb, output_flip_signaled))
		output_flip_signaled(flip, &o->flip_cb);	/* already signalled */
	return 0;
}

/* Desktop frames wait while the client has the primary plane, unless its
 * next frame or a detach gives the plane back. Under the lock. */
static bool output_desktop_held(struct display_output *o)
{
	const uint32_t next = o->client.detach ? 0 :
			      o->client.has_next ? o->client.next.layer : o->client.shown;

	return next == MLG_LX_LAYER_PRIMARY;
}

static bool output_has_work(struct display_output *o)
{
	unsigned long flags;
	bool work;

	spin_lock_irqsave(&o->lock, flags);
	work = (o->next && !output_desktop_held(o)) || o->flip_done || o->client.has_next ||
	       o->client.detach;
	spin_unlock_irqrestore(&o->lock, flags);
	return work || o->stop || kthread_should_stop();
}

/* The detach the worker carried out (or had nothing to do for). */
static void output_detached(struct display_output *o)
{
	unsigned long flags;

	spin_lock_irqsave(&o->lock, flags);
	o->client.detach = false;
	spin_unlock_irqrestore(&o->lock, flags);
	complete(&o->client.detached);
}

static int output_worker(void *arg)
{
	struct display_output *o = arg;

	while (!kthread_should_stop()) {
		struct output_damage damage;
		struct output_layer cl = { 0 };
		struct rt_surface *surface = NULL;
		uint64_t capture_ns = 0, received_ns = 0;
		unsigned long flags;
		bool done, has_cl, detach;
		int r;

		/* Nothing outstanding: sleep until a frame, a client frame, a
		 * detach or a flip arrives. A pending flip is also checked
		 * after a bounded wait, the backstop for a lost flip
		 * interrupt. */
		if (!output_has_work(o))
			output_sleep(o, o->pending_flip ? OUTPUT_FLIP_TIMEOUT_MS : 0);
		if (kthread_should_stop())
			break;
		memset(&damage, 0, sizeof(damage));
		spin_lock_irqsave(&o->lock, flags);
		done = o->flip_done;
		detach = o->client.detach;
		if (o->client.has_next) {
			cl = o->client.next;
			memset(&o->client.next, 0, sizeof(o->client.next));
			o->client.has_next = false;
		}
		has_cl = cl.fb && !detach;
		/* The desktop frame is taken unless the client keeps (or
		 * takes) the primary plane. */
		if (detach || (has_cl ? cl.layer : o->client.shown) != MLG_LX_LAYER_PRIMARY) {
			surface = o->next;
			o->next = NULL;
			damage = o->next_damage;
			memset(&o->next_damage, 0, sizeof(o->next_damage));
			capture_ns = o->next_capture_ns;
			received_ns = o->next_received_ns;
		}
		spin_unlock_irqrestore(&o->lock, flags);
		if (cl.fb && !has_cl)
			layer_drop(&cl);	/* overtaken by the detach */
		if ((done || (o->pending_flip && dma_fence_is_signaled(o->pending_flip))) && o->pending_flip)
			output_flip_account(o);
		if (detach && !o->client.shown && !surface) {
			output_detached(o);
			continue;
		}
		if (!surface && !has_cl && !detach)
			continue;
		if (rt_removal_active(o->adev)) {
			rt_surface_release(surface);
			if (has_cl)
				layer_drop(&cl);
			output_error(o, -ENODEV);
			if (detach)
				output_detached(o);
			break;
		}
		r = output_frame(o, surface, &damage, capture_ns, received_ns, has_cl ? &cl : NULL,
				 detach);
		if (!r && detach) {
			/* The detach returns once the desktop is on screen. */
			r = output_flip_wait(o);
			output_detached(o);
			detach = false;
		}
		if (r) {
			if (has_cl)
				layer_drop(&cl);
			output_error(o, r);
			if (detach)
				output_detached(o);
			break;
		}
	}
	/* Whatever is left in the mailboxes is not shown. */
	{
		struct output_layer cl = { 0 };
		unsigned long flags;
		struct rt_surface *left;

		spin_lock_irqsave(&o->lock, flags);
		left = o->next;
		o->next = NULL;
		if (o->client.has_next) {
			cl = o->client.next;
			memset(&o->client.next, 0, sizeof(o->client.next));
			o->client.has_next = false;
		}
		spin_unlock_irqrestore(&o->lock, flags);
		rt_surface_release(left);
		layer_drop(&cl);
	}
	while (!kthread_should_stop()) {
		unsigned long flags;
		bool detach;

		/* A detach after an error commits nothing: the output's
		 * restore (rt_display_off) puts the screen back. */
		spin_lock_irqsave(&o->lock, flags);
		detach = o->client.detach;
		spin_unlock_irqrestore(&o->lock, flags);
		if (detach)
			output_detached(o);
		output_sleep(o, 1000);
	}
	return 0;
}

static void output_stop(struct display_output *o)
{
	if (o->worker) {
		pthread_mutex_lock(&o->sleep_lock);
		o->stop = true;
		pthread_cond_broadcast(&o->sleep_cond);
		pthread_mutex_unlock(&o->sleep_lock);
		kthread_stop(o->worker);
		o->worker = NULL;
	}
	/* The last flip lands before the screen is restored, unless the
	 * device is gone (nothing will flip). */
	if (o->pending_flip) {
		if (!rt_removal_active(o->adev))
			(void)output_flip_wait(o);
		if (o->pending_flip) {
			dma_fence_remove_callback(o->pending_flip, &o->flip_cb);
			dma_fence_put(o->pending_flip);
			o->pending_flip = NULL;
			for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
				if (o->pending_frame.copy[i]) {
					dma_fence_put(o->pending_frame.copy[i]);
					o->pending_frame.copy[i] = NULL;
				}
		}
	}
}

/* After the screen was restored: the engines, the pins, the buffers. */
static void output_free(struct display_output *o)
{
	output_stop(o);
	/* A client frame the stopped worker never took. */
	if (o->client.has_next) {
		layer_drop(&o->client.next);
		o->client.has_next = false;
	}
	if (o->engines.count)
		rt_surface_engines_fini(&o->engines);
	for (int i = 0; i < OUTPUT_BUFFERS; i++) {
		if (!o->fb[i])
			continue;
		if (o->pinned[i]) {
			struct amdgpu_bo *bo = gem_to_amdgpu_bo(o->fb[i]->fb->obj[0]);

			if (!amdgpu_bo_reserve(bo, true)) {
				amdgpu_bo_unpin(bo);
				amdgpu_bo_unreserve(bo);
			}
		}
		drm_client_buffer_delete(o->fb[i]);
	}
	pthread_cond_destroy(&o->sleep_cond);
	pthread_mutex_destroy(&o->sleep_lock);
	kfree(o);
}

int rt_display_output(struct pci_dev *pdev, const char *connector, uint32_t width,
		      uint32_t height, uint32_t refresh_mhz, struct rt_display_report *report)
{
	struct drm_device *dev = display_device(pdev);
	struct display_output *o;
	struct drm_mode_set *modeset;
	unsigned int lit;
	u64 start;
	int ret;

	report_begin(report);
	if (!dev)
		return -ENODEV;
	if (!connector || !connector[0] || strnlen(connector, RT_DISPLAY_NAME_BYTES) >= RT_DISPLAY_NAME_BYTES ||
	    !width || !height || width > RT_DISPLAY_FB_MAX || height > RT_DISPLAY_FB_MAX || !refresh_mhz)
		return -EINVAL;

	mutex_lock(&rt_display_lock);
	if (rt_display.dev)
		(void)display_off_locked();
	rt_display.restore_status = 0;
	rt_display.commit_ns = rt_display.fill_ns = 0;
	ret = drm_client_init(dev, &rt_display.client, "linuxu-display-output", NULL);
	if (ret) {
		memset(&rt_display.client, 0, sizeof(rt_display.client));
		goto out;
	}
	rt_display.dev = dev;
	rt_display.pattern = RT_DISPLAY_PATTERN_OUTPUT;
	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o) {
		ret = -ENOMEM;
		goto fail;
	}
	rt_display.output = o;
	o->dev = dev;
	o->adev = drm_to_adev(dev);
	o->width = width;
	o->height = height;
	o->front = o->pending = -1;
	init_completion(&o->client.detached);
	spin_lock_init(&o->lock);
	pthread_mutex_init(&o->sleep_lock, NULL);
	pthread_cond_init(&o->sleep_cond, NULL);
	o->stats.version = 2;
	ret = drm_client_modeset_probe(&rt_display.client, 0, 0);
	if (report)
		report->probe_status = ret;
	if (ret)
		goto fail;
	lit = modesets_select(&rt_display.client, connector);
	if (lit != 1) {
		ret = -ENOENT;
		goto fail;
	}
	ret = modeset_use_mode(&rt_display.client, width, height, refresh_mhz);
	if (ret) {
		drm_info(dev, "display output: %s has no %ux%u mode at %u mHz\n", connector, width,
			 height, refresh_mhz);
		goto fail;
	}
	mutex_lock(&rt_display.client.modeset_mutex);
	drm_client_for_each_modeset(modeset, &rt_display.client)
		if (modeset->mode && modeset->num_connectors) {
			o->crtc = modeset->crtc;
			o->connector = modeset->connectors[0];
			o->refresh_mhz = mode_refresh_mhz(modeset->mode);
		}
	mutex_unlock(&rt_display.client.modeset_mutex);
	o->plane = o->crtc ? o->crtc->primary : NULL;
	if (!o->plane) {
		ret = -ENOENT;
		goto fail;
	}
	/* Three buffers, pinned in VRAM for the output's life (amdgpu clears
	 * new dumb buffers with SDMA: they start black). */
	for (int i = 0; i < OUTPUT_BUFFERS; i++) {
		struct amdgpu_bo *bo;

		o->fb[i] = drm_client_buffer_create_dumb(&rt_display.client, width, height,
							 DRM_FORMAT_XRGB8888);
		if (IS_ERR(o->fb[i])) {
			ret = PTR_ERR(o->fb[i]);
			o->fb[i] = NULL;
			goto fail;
		}
		bo = gem_to_amdgpu_bo(o->fb[i]->fb->obj[0]);
		ret = amdgpu_bo_reserve(bo, false);
		if (!ret) {
			ret = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_VRAM);
			if (!ret) {
				o->pinned[i] = true;
				o->fb_address[i] = amdgpu_bo_gpu_offset(bo);
			}
			amdgpu_bo_unreserve(bo);
		}
		if (ret)
			goto fail;
		o->missed[i].full = true;
	}
	ret = rt_surface_engines_init(o->adev, &o->engines);
	if (ret) {
		drm_err(dev, "display output: no SDMA engine to copy with (%d)\n", ret);
		goto fail;
	}
	o->stats.engines = o->engines.count;
	modesets_attach(&rt_display.client, o->fb[0]->fb);
	ret = state_save(dev, &rt_display.saved);
	if (ret)
		goto fail;
	start = ktime_get_ns();
	ret = drm_client_modeset_commit(&rt_display.client);
	rt_display.commit_ns = ktime_get_ns() - start;
	if (report)
		report->commit_status = ret;
	if (ret) {
		drm_err(dev, "display output: commit failed (%d)\n", ret);
		goto fail;
	}
	o->front = 0;
	o->worker = kthread_run(output_worker, o, "display-output");
	if (IS_ERR_OR_NULL(o->worker)) {
		ret = o->worker ? PTR_ERR(o->worker) : -ENOMEM;
		o->worker = NULL;
		goto fail;
	}
	/* Its buffer is drawn by the first frame: the black one stays until then. */
	rt_display.buffer = NULL;
	__atomic_store_n(&rt_display_on, 1, __ATOMIC_RELEASE);
	drm_info(dev, "display output: %s at %ux%u (%u mHz), %d framebuffers, %u SDMA engine(s) (commit %llu ms)\n",
		 connector, width, height, refresh_mhz, OUTPUT_BUFFERS, o->engines.count,
		 rt_display.commit_ns / 1000000);
	report_state(dev, report);
	mutex_unlock(&rt_display_lock);
	return 0;

fail:
	(void)display_off_locked();
out:
	if (report) {
		int probe = report->probe_status, commit = report->commit_status;

		report_state(dev, report);
		report->probe_status = probe;
		report->commit_status = commit;
	}
	mutex_unlock(&rt_display_lock);
	return ret;
}

int rt_display_present(struct pci_dev *pdev, struct rt_surface *surface,
		       const struct rt_surface_rect *rects, uint32_t count, uint64_t capture_ns,
		       struct rt_display_present_stats *stats)
{
	struct drm_device *dev = display_device(pdev);
	struct display_output *o;
	struct rt_surface *replaced = NULL;
	uint32_t width, height, pitch;
	unsigned long flags;
	int ret = 0;

	if (stats)
		memset(stats, 0, sizeof(*stats));
	if (!dev) {
		rt_surface_release(surface);
		return -ENODEV;
	}
	if (!surface || (count && !rects)) {
		rt_surface_release(surface);
		return -EINVAL;
	}
	mutex_lock(&rt_display_lock);
	o = rt_display.dev == dev ? rt_display.output : NULL;
	if (!o) {
		mutex_unlock(&rt_display_lock);
		rt_surface_release(surface);
		return -ENOENT;
	}
	rt_surface_geometry(surface, &width, &height, &pitch);
	spin_lock_irqsave(&o->lock, flags);
	if (o->stats.error) {
		ret = o->stats.error;
	} else if (width != o->width || height != o->height) {
		ret = -EINVAL;
	} else if (count) {
		struct output_damage add = { .count = 0 };

		/* The newest frame replaces one not yet taken; damage adds up. */
		replaced = o->next;
		if (replaced)
			o->stats.frames_replaced++;
		damage_add(&add, rects, count);
		damage_merge(&o->next_damage, &add);
		o->next = surface;
		surface = NULL;
		o->next_capture_ns = capture_ns;
		o->next_received_ns = ktime_get_ns();
		o->stats.frames_received++;
		if (o->client.primary_owned)
			o->client.desktop_held++;	/* waits for the primary plane */
	}
	if (stats)
		*stats = o->stats;
	spin_unlock_irqrestore(&o->lock, flags);
	if (stats) {
		struct linuxu_aperture_stats ap;

		linuxu_aperture_stats(&ap);
		stats->aperture_ops = ap.stores + ap.loads;
	}
	if (!surface)
		output_kick(o);	/* under rt_display_lock: o cannot go meanwhile */
	mutex_unlock(&rt_display_lock);
	rt_surface_release(surface);
	rt_surface_release(replaced);
	return ret;
}

int rt_display_stats(struct pci_dev *pdev, struct rt_display_present_stats *stats)
{
	struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;
	struct display_output *o;
	unsigned long flags;
	int ret = -ENOENT;

	if (!stats)
		return -EINVAL;
	memset(stats, 0, sizeof(*stats));
	mutex_lock(&rt_display_lock);
	o = dev && rt_display.dev == dev ? rt_display.output : NULL;
	if (o) {
		struct linuxu_aperture_stats ap;

		spin_lock_irqsave(&o->lock, flags);
		*stats = o->stats;
		spin_unlock_irqrestore(&o->lock, flags);
		linuxu_aperture_stats(&ap);
		stats->aperture_ops = ap.stores + ap.loads;
		ret = 0;
	}
	mutex_unlock(&rt_display_lock);
	return ret;
}

int rt_display_showing(void)
{
	return __atomic_load_n(&rt_display_on, __ATOMIC_ACQUIRE);
}

/* ---- a client's framebuffers on the output (rt_display_lx_hooks) ----
 *
 * A Linux-file client (rt/lx_files.h) opens the primary node, never as DRM
 * master, creates framebuffers there and hands them to the output's worker
 * with LX_SCANOUT (rt/lx_abi.h). The worker stays the only committer: a
 * client frame goes into its next commit, on the primary plane in place of
 * the desktop or on the overlay plane reserved for the client, and the
 * client's syncobj gets that flip's fence. */

extern int drm_dropmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

static void lx_primary_lock(void)
{
	mutex_lock(&rt_display_lock);
}

static void lx_primary_unlock(void)
{
	mutex_unlock(&rt_display_lock);
}

/* drm_open made the file master when no master existed. A client must not
 * be: the driver's own modesets (drm_client_modeset_commit) fail while any
 * file is master, and KMS that changes the screen is the output's. */
static int lx_primary_opened(struct drm_device *dev, struct file *file)
{
	struct drm_file *fp = file ? file->private_data : NULL;
	int ret;

	if (!fp)
		return -EINVAL;
	if (!drm_is_current_master(fp))
		return 0;
	ret = drm_dropmaster_ioctl(dev, NULL, fp);
	if (ret)
		drm_err(dev, "display: a client's primary-node file stays DRM master (%d)\n", ret);
	return ret;
}

/* The output for @pdev, under rt_display_lock. */
static struct display_output *scanout_output(struct pci_dev *pdev)
{
	struct drm_device *dev = display_device(pdev);

	return dev && rt_display.dev == dev ? rt_display.output : NULL;
}

static void scanout_state(struct display_output *o, void *owner, struct mlg_lx_scanout_state *st)
{
	unsigned long flags;

	if (!o)
		return;
	st->output = 1;
	st->connector_id = o->connector ? o->connector->base.id : 0;
	if (o->connector)
		strscpy(st->connector, o->connector->name, sizeof(st->connector));
	st->crtc_id = o->crtc->base.id;
	st->primary_plane_id = o->plane->base.id;
	st->width = o->width;
	st->height = o->height;
	st->refresh_mhz = o->refresh_mhz;
	if (o->client.owner != owner)
		return;
	st->attached = 1;
	st->overlay_plane_id = o->client.overlay ? o->client.overlay->base.id : 0;
	spin_lock_irqsave(&o->lock, flags);
	st->error = o->stats.error;
	st->layer = o->client.last_layer;
	st->presents = o->client.presents;
	st->flips = o->client.flips;
	st->desktop_held = o->client.desktop_held;
	spin_unlock_irqrestore(&o->lock, flags);
}

static bool fb_owned(struct drm_file *fp, struct drm_framebuffer *fb)
{
	struct drm_framebuffer *it;
	bool found = false;

	mutex_lock(&fp->fbs_lock);
	list_for_each_entry(it, &fp->fbs, filp_head)
		if (it == fb) {
			found = true;
			break;
		}
	mutex_unlock(&fp->fbs_lock);
	return found;
}

/* A client frame from a request: its framebuffer (of the client's file
 * @fp), rectangles and, with @sync, its syncobj. 0 with references in
 * @cl, or -errno. */
static int scanout_layer(struct display_output *o, struct drm_file *fp,
			 const struct mlg_lx_scanout *req, bool sync, struct output_layer *cl)
{
	struct drm_framebuffer *fb;

	memset(cl, 0, sizeof(*cl));
	if (req->layer != MLG_LX_LAYER_PRIMARY && req->layer != MLG_LX_LAYER_OVERLAY)
		return -EINVAL;
	if (req->layer == MLG_LX_LAYER_OVERLAY && !o->client.overlay)
		return -ENXIO;	/* no overlay plane was free for this CRTC */
	fb = drm_framebuffer_lookup(o->dev, fp, req->fb_id);
	if (!fb)
		return -ENOENT;
	if (!fb_owned(fp, fb)) {
		drm_framebuffer_put(fb);
		return -EPERM;
	}
	cl->layer = req->layer;
	cl->fb = fb;
	if (req->src_w && req->src_h) {
		cl->src_x = req->src_x;
		cl->src_y = req->src_y;
		cl->src_w = req->src_w;
		cl->src_h = req->src_h;
	} else {
		cl->src_w = fb->width;
		cl->src_h = fb->height;
	}
	if ((u64)cl->src_x + cl->src_w > fb->width || (u64)cl->src_y + cl->src_h > fb->height ||
	    cl->src_w > 0xffff || cl->src_h > 0xffff) {
		layer_drop(cl);
		return -ERANGE;
	}
	if (req->dst_w && req->dst_h) {
		cl->dst_x = req->dst_x;
		cl->dst_y = req->dst_y;
		cl->dst_w = req->dst_w;
		cl->dst_h = req->dst_h;
	} else {
		cl->dst_w = o->width;
		cl->dst_h = o->height;
	}
	if (sync) {
		cl->sync = drm_syncobj_find(fp, req->syncobj);
		if (!cl->sync) {
			layer_drop(cl);
			return -ENOENT;
		}
	}
	return 0;
}

/* Take the client off the screen through the worker: returns once the
 * desktop is back (or the worker reported why not). rt_display_lock. */
static int scanout_detach(struct display_output *o)
{
	unsigned long flags;
	bool busy;
	int ret;

	spin_lock_irqsave(&o->lock, flags);
	busy = o->client.has_next || o->client.primary_owned || o->client.shown;
	if (busy) {
		reinit_completion(&o->client.detached);
		o->client.detach = true;
	}
	spin_unlock_irqrestore(&o->lock, flags);
	if (busy && o->worker) {
		output_kick(o);
		if (!wait_for_completion_timeout(&o->client.detached,
						 msecs_to_jiffies(2 * OUTPUT_COPY_TIMEOUT_MS))) {
			drm_err(o->dev, "display: the client's frame did not leave the screen in %u ms\n",
				2 * OUTPUT_COPY_TIMEOUT_MS);
			return -ETIME;
		}
	}
	spin_lock_irqsave(&o->lock, flags);
	ret = o->stats.error;
	o->client.detach = false;
	spin_unlock_irqrestore(&o->lock, flags);
	return ret;
}

static void scanout_release(struct display_output *o)
{
	o->client.owner = NULL;
	o->client.overlay = NULL;
}

static int lx_scanout(struct pci_dev *pdev, void *owner, struct file *file,
		      const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state)
{
	struct drm_file *fp = file ? file->private_data : NULL;
	struct display_output *o;
	struct output_layer cl;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&rt_display_lock);
	o = scanout_output(pdev);
	switch (req->op) {
	case MLG_LX_SCANOUT_STATE:
		break;
	case MLG_LX_SCANOUT_ATTACH:
		if (!o) {
			ret = -ENOENT;
		} else if (req->connector[0] && (!o->connector || !connector_named(o->connector,
										   req->connector))) {
			ret = -ENOENT;
		} else if (o->client.owner && o->client.owner != owner) {
			ret = -EBUSY;
		} else if (!o->client.owner) {
			struct drm_plane *plane;

			o->client.owner = owner;
			o->client.overlay = NULL;
			drm_for_each_plane(plane, o->dev) {
				if (plane->type != DRM_PLANE_TYPE_OVERLAY ||
				    !(plane->possible_crtcs & drm_crtc_mask(o->crtc)) ||
				    (plane->state && plane->state->crtc))
					continue;
				o->client.overlay = plane;
				break;
			}
			spin_lock_irqsave(&o->lock, flags);
			o->client.presents = o->client.flips = o->client.desktop_held = 0;
			o->client.last_layer = 0;
			spin_unlock_irqrestore(&o->lock, flags);
		}
		break;
	case MLG_LX_SCANOUT_TEST:
	case MLG_LX_SCANOUT_PRESENT:
		if (!o) {
			ret = -ENOENT;
			break;
		}
		if (o->client.owner != owner) {
			ret = -EACCES;
			break;
		}
		if (!fp) {
			ret = -EBADF;
			break;
		}
		ret = scanout_layer(o, fp, req, req->op == MLG_LX_SCANOUT_PRESENT, &cl);
		if (ret)
			break;
		if (req->op == MLG_LX_SCANOUT_TEST) {
			ret = output_check(o, &cl);
			layer_drop(&cl);
			break;
		}
		/* Reset now: the fence it gets is the flip's. */
		drm_syncobj_replace_fence(cl.sync, NULL);
		spin_lock_irqsave(&o->lock, flags);
		if (o->stats.error) {
			ret = o->stats.error;
		} else if (o->client.has_next) {
			ret = -EBUSY;
		} else {
			o->client.next = cl;
			o->client.has_next = true;
			o->client.presents++;
			o->client.last_layer = cl.layer;
			memset(&cl, 0, sizeof(cl));
		}
		spin_unlock_irqrestore(&o->lock, flags);
		layer_drop(&cl);
		if (!ret)
			output_kick(o);
		break;
	case MLG_LX_SCANOUT_DETACH:
		if (o && o->client.owner == owner) {
			ret = scanout_detach(o);
			scanout_release(o);
		} else if (o && o->client.owner) {
			ret = -EACCES;
		}
		break;
	case MLG_LX_SCANOUT_HIDE:
		/* Off the screen, still attached. */
		if (!o)
			ret = -ENOENT;
		else if (o->client.owner != owner)
			ret = -EACCES;
		else
			ret = scanout_detach(o);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	scanout_state(o, owner, state);
	mutex_unlock(&rt_display_lock);
	return ret;
}

static void lx_client_gone(struct pci_dev *pdev, void *owner)
{
	struct display_output *o;

	mutex_lock(&rt_display_lock);
	o = scanout_output(pdev);
	if (o && o->client.owner == owner) {
		int ret = scanout_detach(o);

		if (ret)
			drm_err(o->dev, "display: taking a closed client's frame off failed (%d)\n", ret);
		scanout_release(o);
	}
	mutex_unlock(&rt_display_lock);
}

const struct rt_lx_display_hooks rt_display_lx_hooks = {
	.primary_lock = lx_primary_lock,
	.primary_unlock = lx_primary_unlock,
	.primary_opened = lx_primary_opened,
	.scanout = lx_scanout,
	.client_gone = lx_client_gone,
};
