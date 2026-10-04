/* drmMlgPlaceWindow (libdrm-mlg): where a window's frames go on the
 * output. macOS global points (top left origin) to CRTC pixels for screens
 * at different global positions and backing scales, the whole-screen and
 * native full screen cases, clipping with the matching image crop on every
 * edge, a window off the screen, a sliver under DC's minimum viewport, the
 * modes, no screen, and refusals. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); exit(1); } } while (0)

static struct drm_mlg_place_in dell(double sx, double sy, double sw, double sh)
{
	return (struct drm_mlg_place_in){
		.mode = DRM_MLG_PLACE_AUTO, .have_screen = 1,
		.screen_x = sx, .screen_y = sy, .screen_w = sw, .screen_h = sh,
		.have_window = 1, .crtc_w = 2560, .crtc_h = 1440, .image_w = 1920, .image_h = 1080,
	};
}

static void window(struct drm_mlg_place_in *in, double x, double y, double w, double h)
{
	in->win_x = x;
	in->win_y = y;
	in->win_w = w;
	in->win_h = h;
}

static int is(const struct drm_mlg_placement *p, uint32_t layer, int32_t dx, int32_t dy, uint32_t dw,
	      uint32_t dh, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh)
{
	if (p->layer == layer && p->dst_x == dx && p->dst_y == dy && p->dst_w == dw && p->dst_h == dh &&
	    p->src_x == sx && p->src_y == sy && p->src_w == sw && p->src_h == sh)
		return 1;
	fprintf(stderr, "got layer %u dst %d,%d %ux%u src %u,%u %ux%u (%s)\n", p->layer, p->dst_x,
		p->dst_y, p->dst_w, p->dst_h, p->src_x, p->src_y, p->src_w, p->src_h, p->why);
	return 0;
}

int main(void)
{
	struct drm_mlg_placement p;
	struct drm_mlg_place_in in;

	/* The Dell right of the built-in screen, at 1x: a 1920x1080 window
	 * at 320,180 on it is an overlay there, the whole image. */
	in = dell(2336, 0, 2560, 1440);
	window(&in, 2336 + 320, 180, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 320, 180, 1920, 1080, 0, 0, 1920, 1080));

	/* The same screen at 2x (1280x720 points on the 2560x1440 mode): a
	 * point is two pixels. */
	in = dell(2336, 0, 1280, 720);
	in.image_w = 1280;
	in.image_h = 720;
	window(&in, 2336 + 100, 50, 640, 360);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 200, 100, 1280, 720, 0, 0, 1280, 720));

	/* The Dell left of and above the main screen (negative coordinates). */
	in = dell(-2560, -300, 2560, 1440);
	window(&in, -2560 + 10, -300 + 20, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 10, 20, 1920, 1080, 0, 0, 1920, 1080));

	/* Hanging off the right edge by half: the left half of the image. */
	in = dell(2336, 0, 2560, 1440);
	window(&in, 2336 + 2560 - 960, 100, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 1600, 100, 960, 1080, 0, 0, 960, 1080));
	/* ... off the left edge (onto the built-in screen): the right part. */
	window(&in, 2336 - 480, 100, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 0, 100, 1440, 1080, 480, 0, 1440, 1080));
	/* ... off the top and the bottom. */
	window(&in, 2336 + 100, -540, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 100, 0, 1920, 540, 0, 540, 1920, 540));
	window(&in, 2336 + 100, 1440 - 300, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 100, 1140, 1920, 300, 0, 0, 1920, 300));
	/* A corner off both edges. */
	window(&in, 2336 - 960, -540, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 0, 0, 960, 540, 960, 540, 960, 540));

	/* A window drawn at half the image's size (the image scaled down 2:1),
	 * half off the right edge: the crop is in image pixels. */
	window(&in, 2336 + 2560 - 480, 100, 960, 540);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 2080, 100, 480, 540, 0, 0, 960, 1080));

	/* Entirely off: nothing, with the reason. */
	window(&in, 100, 100, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == 0 && strstr(p.why, "off"));
	window(&in, 2336 + 2560, 0, 640, 480);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == 0);
	/* A sliver of 5 pixels: under DC's 12-pixel viewport, not grown. */
	window(&in, 2336 - 1920 + 5, 100, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == 0 && strstr(p.why, "12-pixel"));
	window(&in, 2336 - 1920 + 12, 100, 1920, 1080);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 0, 100, 12, 1080, 1908, 0, 12, 1080));

	/* Filling the screen: the primary plane, whole; native full screen
	 * too; MODE_WINDOWED keeps such a window on the overlay. */
	window(&in, 2336, 0, 2560, 1440);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_PRIMARY, 0, 0, 2560, 1440, 0, 0, 1920, 1080));
	in.mode = DRM_MLG_PLACE_WINDOWED;
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 0, 0, 2560, 1440, 0, 0, 1920, 1080));
	in.mode = DRM_MLG_PLACE_AUTO;
	window(&in, 2336 + 100, 100, 800, 600);
	in.window_native_fullscreen = 1;
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == MLG_LX_LAYER_PRIMARY);
	in.window_native_fullscreen = 0;
	/* MODE_FULLSCREEN: the primary plane wherever the window is. */
	in.mode = DRM_MLG_PLACE_FULLSCREEN;
	window(&in, 100, 100, 640, 480);
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == MLG_LX_LAYER_PRIMARY);

	/* No macOS screen for the monitor: full screen, or the image centred
	 * when windowed. */
	in = dell(0, 0, 0, 0);
	in.have_screen = 0;
	CHECK(drmMlgPlaceWindow(&in, &p) == 0 && p.layer == MLG_LX_LAYER_PRIMARY);
	in.mode = DRM_MLG_PLACE_WINDOWED;
	CHECK(drmMlgPlaceWindow(&in, &p) == 0);
	CHECK(is(&p, MLG_LX_LAYER_OVERLAY, 320, 180, 1920, 1080, 0, 0, 1920, 1080));

	/* Refusals. */
	in = dell(2336, 0, 2560, 1440);
	in.crtc_w = 0;
	CHECK(drmMlgPlaceWindow(&in, &p) == -EINVAL && p.why[0]);
	in = dell(2336, 0, 0, 1440);
	CHECK(drmMlgPlaceWindow(&in, &p) == -EINVAL);
	in = dell(2336, 0, 2560, 1440);
	in.mode = 7;
	CHECK(drmMlgPlaceWindow(&in, &p) == -EINVAL);

	printf("PASS mlg place: points to CRTC pixels at 1x and 2x and any global position, "
	       "full screen, clipping with the image crop on every edge, off the screen, the "
	       "12-pixel viewport, modes, no screen, refusals\n");
	return 0;
}
