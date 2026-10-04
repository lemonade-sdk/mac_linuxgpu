/* mlg-run: run a Vulkan program so that it renders on the AMD GPU with
 * RADV and the GPU itself scans its frames out on the monitor the driver's
 * display output drives, whatever the program would pick on its own.
 *
 *   mlg-run [--output CONNECTOR|auto] [--mode auto|fullscreen|windowed]
 *           [--quake3 [--basepath DIR] [--basegame NAME]]
 *           [--icd RADEON_ICD.json] [--loader libvulkan.1.dylib]
 *           [--dry-run] -- PROGRAM [ARGS...]
 *
 * It finds the RADV ICD and the Vulkan loader, asks the driver which
 * output runs (and checks --output against it) and which macOS display
 * stands for its monitor, then runs PROGRAM with the environment of
 * plan.c: only RADV for the loader, the loader for SDL, MLG_WSI_OUTPUT and
 * MLG_WSI_MODE for RADV's presentation, SDL's full screen on its own
 * screen. --quake3 adds Quake3e's settings that put its window on that
 * screen in that mode; --basepath and --basegame name the game data (for
 * OpenArena: the directory that holds baseoa, and baseoa), which must hold
 * .pk3 files. --dry-run prints all that and runs nothing.
 *
 * Every missing piece is an error that says what is missing; nothing is
 * run in its place. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <CoreGraphics/CoreGraphics.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "plan.h"

static void usage(void)
{
	fprintf(stderr,
		"usage: mlg-run [--output CONNECTOR|auto] [--mode auto|fullscreen|windowed]\n"
		"               [--quake3 [--basepath DIR] [--basegame NAME]]\n"
		"               [--icd RADEON_ICD.json] [--loader libvulkan.1.dylib] [--dry-run]\n"
		"               -- PROGRAM [ARGS...]\n");
}

static bool exists(const char *path)
{
	return path && access(path, R_OK) == 0;
}

/* The RADV ICD manifest: --icd, $MLG_RADV_ICD, or the one built next to
 * this program (build/mlg-run -> build/radv/install/...). */
static int find_icd(const char *given, char *out, size_t size)
{
	char exe[PATH_MAX], real[PATH_MAX];
	uint32_t n = sizeof(exe);

	if (!given)
		given = getenv("MLG_RADV_ICD");
	if (given) {
		if (!exists(given) || !realpath(given, out)) {
			fprintf(stderr, "mlg-run: the RADV ICD %s does not exist\n", given);
			return -1;
		}
		return 0;
	}
	if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) {
		fprintf(stderr, "mlg-run: cannot tell where mlg-run is; pass --icd\n");
		return -1;
	}
	char *slash = strrchr(real, '/');
	*slash = 0;
	snprintf(out, size, "%s/radv/install/share/vulkan/icd.d/radeon_icd.json", real);
	if (!exists(out)) {
		fprintf(stderr, "mlg-run: no RADV ICD at %s (scripts/build-radv.sh); pass --icd\n", out);
		return -1;
	}
	return 0;
}

/* The Khronos Vulkan loader: --loader, $MLG_VULKAN_LOADER, Homebrew's or the
 * Vulkan SDK's. */
static int find_loader(const char *given, char *out, size_t size)
{
	const char *sdk = getenv("VULKAN_SDK");
	char sdk_path[PATH_MAX];
	const char *candidates[] = {
		"/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib",
		"/opt/homebrew/lib/libvulkan.1.dylib",
		"/usr/local/opt/vulkan-loader/lib/libvulkan.1.dylib",
		"/usr/local/lib/libvulkan.1.dylib",
		NULL,
	};

	if (!given)
		given = getenv("MLG_VULKAN_LOADER");
	if (given) {
		if (!exists(given) || !realpath(given, out)) {
			fprintf(stderr, "mlg-run: the Vulkan loader %s does not exist\n", given);
			return -1;
		}
		return 0;
	}
	if (sdk) {
		snprintf(sdk_path, sizeof(sdk_path), "%s/lib/libvulkan.1.dylib", sdk);
		candidates[4] = sdk_path;
	}
	for (int i = 0; i < 5; i++) {
		if (candidates[i] && exists(candidates[i])) {
			snprintf(out, size, "%s", candidates[i]);
			return 0;
		}
	}
	fprintf(stderr, "mlg-run: no Vulkan loader found (brew install vulkan-loader); pass --loader\n");
	return -1;
}

/* Quake3e's game data: @basepath/@basegame (baseq3 without --basegame)
 * holds .pk3 files. The resolved base path goes to @out. */
static int check_game_data(const char *basepath, const char *basegame, char *out, size_t size)
{
	char dir[PATH_MAX];
	DIR *d;
	struct dirent *e;
	bool pk3 = false;

	if (!realpath(basepath, out)) {
		fprintf(stderr, "mlg-run: --basepath %s: %s\n", basepath, strerror(errno));
		return -1;
	}
	(void)size;
	snprintf(dir, sizeof(dir), "%s/%s", out, basegame ? basegame : "baseq3");
	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "mlg-run: no game directory %s: %s\n", dir, strerror(errno));
		return -1;
	}
	while ((e = readdir(d)) && !pk3) {
		size_t n = strlen(e->d_name);

		pk3 = n > 4 && !strcasecmp(e->d_name + n - 4, ".pk3");
	}
	closedir(d);
	if (!pk3) {
		fprintf(stderr, "mlg-run: %s holds no .pk3 game data\n", dir);
		return -1;
	}
	return 0;
}

/* A plane's "type" and how many XRGB8888 modifiers its IN_FORMATS lists. */
static void plane_info(int card, uint32_t plane, uint64_t *type, unsigned *xrgb_mods)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(card, plane, DRM_MODE_OBJECT_PLANE);

	*type = ~0ull;
	*xrgb_mods = 0;
	for (uint32_t i = 0; props && i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(card, props->props[i]);

		if (p && !strcmp(p->name, "type"))
			*type = props->prop_values[i];
		if (p && !strcmp(p->name, "IN_FORMATS")) {
			drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(card, (uint32_t)props->prop_values[i]);
			drmModeFormatModifierIterator it = { 0 };

			while (blob && drmModeFormatModifierBlobIterNext(blob, &it))
				*xrgb_mods += it.fmt == 0x34325258u;	/* DRM_FORMAT_XRGB8888 */
			drmModeFreePropertyBlob(blob);
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

/* What the output has to scan a program's frames out with: its CRTC, the
 * primary plane, the overlay planes the CRTC can use, and the monitor's
 * EDID identity (what its macOS display is matched by). */
static void describe_output(int card, const struct mlg_lx_scanout_state *st)
{
	drmModeResPtr res = drmModeGetResources(card);
	drmModePlaneResPtr planes;

	/* Primary and cursor planes are listed only to universal-plane clients. */
	if (drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
		fprintf(stderr, "  (universal planes refused: %s)\n", strerror(errno));
	planes = drmModeGetPlaneResources(card);
	int crtc_index = -1;

	fprintf(stderr, "  output: %s (connector %u), CRTC %u, %ux%u at %u.%03u Hz\n", st->connector,
		st->connector_id, st->crtc_id, st->width, st->height, st->refresh_mhz / 1000,
		st->refresh_mhz % 1000);
	for (int i = 0; res && i < res->count_crtcs; i++)
		if (res->crtcs[i] == st->crtc_id)
			crtc_index = i;
	for (uint32_t i = 0; planes && i < planes->count_planes; i++) {
		drmModePlanePtr p = drmModeGetPlane(card, planes->planes[i]);
		uint64_t type;
		unsigned mods;

		if (!p)
			continue;
		if (crtc_index >= 0 && (p->possible_crtcs & (1u << crtc_index))) {
			plane_info(card, p->plane_id, &type, &mods);
			if (type == 1 || type == 0)
				fprintf(stderr, "  plane %u: %s, %s, %u XRGB8888 modifiers\n", p->plane_id,
					type == 1 ? "primary" : "overlay",
					p->crtc_id ? "in use" : "free", mods);
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);

	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(card, st->connector_id,
								      DRM_MODE_OBJECT_CONNECTOR);
	for (uint32_t i = 0; props && i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(card, props->props[i]);

		if (p && !strcmp(p->name, "EDID") && props->prop_values[i]) {
			drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(card, (uint32_t)props->prop_values[i]);
			uint32_t vendor, product, serial;

			if (blob && !drmMlgEdidIdentity(blob->data, blob->length, &vendor, &product, &serial))
				fprintf(stderr, "  EDID identity: vendor %c%c%c (0x%04x), product 0x%04x, serial %u\n",
					'@' + ((vendor >> 10) & 31), '@' + ((vendor >> 5) & 31), '@' + (vendor & 31),
					vendor, product, serial);
			drmModeFreePropertyBlob(blob);
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

/* The display output and its macOS display, from the driver. */
static int find_output(const char *wanted, struct plan_input *in, char *connector, size_t size)
{
	struct mlg_lx_scanout req = { .version = MLG_LX_SCANOUT_VERSION, .op = MLG_LX_SCANOUT_STATE };
	struct mlg_lx_scanout_state st;
	uint32_t display = 0;
	int card, r;

	card = drmFileOpen("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (card < 0) {
		fprintf(stderr, "mlg-run: opening the GPU's primary node failed: %s "
			"(is the MacLinuxGPU driver running?)\n", strerror(errno));
		return -1;
	}
	r = drmMlgScanout(&req, &st);
	if (r) {
		fprintf(stderr, "mlg-run: asking the driver for its display output failed: %s\n",
			strerror(-r));
		close(card);
		return -1;
	}
	if (!st.output) {
		fprintf(stderr, "mlg-run: the driver has no display output; the display agent lights "
			"one (MacLinuxGPUHost display-agent)\n");
		close(card);
		return -1;
	}
	if (strcmp(wanted, "auto") && strcmp(wanted, st.connector)) {
		fprintf(stderr, "mlg-run: --output %s: the display output drives %s\n", wanted,
			st.connector);
		close(card);
		return -1;
	}
	snprintf(connector, size, "%s", st.connector);
	in->mode_width = st.width;
	in->mode_height = st.height;
	describe_output(card, &st);
	r = drmMlgConnectorDisplay(card, st.connector_id, &display);
	close(card);
	if (r == -ENOENT) {
		fprintf(stderr, "mlg-run: no macOS display stands for %s: the program's window stays "
			"where it opens, and its frames go full screen on %s\n", connector, connector);
		in->have_display = false;
		return 0;
	}
	if (r) {
		fprintf(stderr, "mlg-run: finding the macOS display of %s failed: %s\n", connector,
			strerror(-r));
		return -1;
	}
	CGRect bounds = CGDisplayBounds(display);
	fprintf(stderr, "  macOS display %u (vendor 0x%04x, model 0x%04x, serial %u): %.0fx%.0f at %.0f,%.0f\n",
		display, CGDisplayVendorNumber(display), CGDisplayModelNumber(display),
		CGDisplaySerialNumber(display), bounds.size.width, bounds.size.height, bounds.origin.x,
		bounds.origin.y);
	in->have_display = true;
	in->display_x = (int32_t)bounds.origin.x;
	in->display_y = (int32_t)bounds.origin.y;
	in->display_width = (uint32_t)bounds.size.width;
	in->display_height = (uint32_t)bounds.size.height;
	return 0;
}

int main(int argc, char **argv)
{
	const char *output = "auto", *icd_arg = NULL, *loader_arg = NULL, *basepath_arg = NULL;
	char icd[PATH_MAX], loader[PATH_MAX], basepath[PATH_MAX], connector[MLG_LX_SCANOUT_NAME_BYTES], why[160];
	struct plan_input in = { .mode = PLAN_MODE_AUTO };
	static struct plan plan;
	bool dry_run = false;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		if (!strcmp(a, "--quake3")) {
			in.quake3 = true;
		} else if (!strcmp(a, "--dry-run")) {
			dry_run = true;
		} else if (!strcmp(a, "--output") && i + 1 < argc) {
			output = argv[++i];
		} else if (!strcmp(a, "--mode") && i + 1 < argc) {
			if (plan_mode_parse(argv[++i], &in.mode)) {
				fprintf(stderr, "mlg-run: --mode %s: auto, fullscreen or windowed\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(a, "--basepath") && i + 1 < argc) {
			basepath_arg = argv[++i];
		} else if (!strcmp(a, "--basegame") && i + 1 < argc) {
			in.basegame = argv[++i];
		} else if (!strcmp(a, "--icd") && i + 1 < argc) {
			icd_arg = argv[++i];
		} else if (!strcmp(a, "--loader") && i + 1 < argc) {
			loader_arg = argv[++i];
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else {
			usage();
			return 2;
		}
	}
	if (i >= argc) {
		usage();
		return 2;
	}
	in.argc = argc - i;
	in.argv = argv + i;

	if (basepath_arg) {
		if (check_game_data(basepath_arg, in.basegame, basepath, sizeof(basepath)))
			return 1;
		in.basepath = basepath;
	}
	if (find_icd(icd_arg, icd, sizeof(icd)) || find_loader(loader_arg, loader, sizeof(loader)) ||
	    find_output(output, &in, connector, sizeof(connector)))
		return 1;
	in.icd_json = icd;
	in.loader = loader;
	in.connector = connector;
	if (plan_make(&in, &plan, why, sizeof(why))) {
		fprintf(stderr, "mlg-run: %s\n", why);
		return 1;
	}

	fprintf(stderr, "mlg-run: %s on %s (%ux%u)%s, %s\n", in.argv[0], connector, in.mode_width,
		in.mode_height, in.have_display ? "" : " (no macOS display)", plan_mode_name(in.mode));
	for (int e = 0; e < plan.env_count; e++)
		fprintf(stderr, "  %s=%s\n", plan.env_name[e], plan.env_value[e]);
	fprintf(stderr, "  ");
	for (int a = 0; a < plan.argc; a++)
		fprintf(stderr, "%s%s", a ? " " : "", plan.argv[a]);
	fprintf(stderr, "\n");
	if (dry_run)
		return 0;
	for (int e = 0; e < plan.env_count; e++)
		if (setenv(plan.env_name[e], plan.env_value[e], 1)) {
			fprintf(stderr, "mlg-run: setenv %s failed: %s\n", plan.env_name[e], strerror(errno));
			return 1;
		}
	execvp(plan.argv[0], plan.argv);
	fprintf(stderr, "mlg-run: running %s failed: %s\n", plan.argv[0], strerror(errno));
	return 127;
}
