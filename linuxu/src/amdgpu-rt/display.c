/* The in-driver display test (rt/display.h): a DRM client that shows a
 * static pattern through upstream drm_client, the atomic helpers and the
 * driver's own KMS (amdgpu_dm and Display Core on amdgpu). */
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
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
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <rt/display.h>

#include "amdgpu.h"

/* The largest framebuffer the test creates: two 4K monitors side by side
 * would not fit, one 8K mode does. */
#define RT_DISPLAY_FB_MAX 8192u

/* scripts/display-test.py decodes this layout. */
_Static_assert(sizeof(struct rt_display_connector) == 80, "rt_display_connector layout");
_Static_assert(sizeof(struct rt_display_report) == 72 + 8 * 80, "rt_display_report layout");

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

/* Guarded by rt_display_lock. */
static struct {
	struct drm_device *dev;
	struct drm_client_dev client;
	struct drm_client_buffer *buffer;
	struct display_snapshot *saved;	/* the configuration before the pattern */
	uint32_t pattern;
	uint64_t fill_ns, commit_ns;
	int restore_status;
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
	if (report->showing && rt_display.buffer) {
		struct drm_framebuffer *fb = rt_display.buffer->fb;
		struct amdgpu_bo *bo = gem_to_amdgpu_bo(fb->obj[0]);

		report->pattern = rt_display.pattern;
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

int rt_display_showing(void)
{
	return __atomic_load_n(&rt_display_on, __ATOMIC_ACQUIRE);
}
