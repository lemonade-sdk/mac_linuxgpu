/* mlg-run's plan (plan.c): the environment and the command line for each
 * mode, with and without a macOS display, for Quake3e and any program, and
 * the refusals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plan.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); exit(1); } } while (0)

static const char *env_of(const struct plan *p, const char *name)
{
	for (int i = 0; i < p->env_count; i++)
		if (!strcmp(p->env_name[i], name))
			return p->env_value[i];
	return NULL;
}

/* The value after "+set name", or NULL. */
static const char *setting(const struct plan *p, const char *name)
{
	for (int i = 0; i + 2 < p->argc; i++)
		if (!strcmp(p->argv[i], "+set") && !strcmp(p->argv[i + 1], name))
			return p->argv[i + 2];
	return NULL;
}

int main(void)
{
	static struct plan p;
	char *game[] = { "./quake3e.arm64", "+map", "q3dm17", NULL };
	char *vkcube[] = { "vkcube", NULL };
	char why[160];
	struct plan_input in = {
		.icd_json = "/x/radeon_icd.json", .loader = "/y/libvulkan.1.dylib",
		.connector = "DP-1", .mode = PLAN_MODE_AUTO, .quake3 = true,
		.have_display = true, .display_x = 1512, .display_y = -300,
		.display_width = 2560, .display_height = 1440,
		.mode_width = 2560, .mode_height = 1440, .argc = 3, .argv = game,
	};
	enum plan_mode mode;

	/* Modes by name. */
	CHECK(!plan_mode_parse("auto", &mode) && mode == PLAN_MODE_AUTO);
	CHECK(!plan_mode_parse("fullscreen", &mode) && mode == PLAN_MODE_FULLSCREEN);
	CHECK(!plan_mode_parse("windowed", &mode) && mode == PLAN_MODE_WINDOWED);
	CHECK(plan_mode_parse("window", &mode) == -1);

	/* Quake3e, full screen on the monitor's screen. */
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(!strcmp(env_of(&p, "VK_DRIVER_FILES"), "/x/radeon_icd.json"));
	CHECK(!strcmp(env_of(&p, "VK_ICD_FILENAMES"), "/x/radeon_icd.json"));
	CHECK(!strcmp(env_of(&p, "SDL_VULKAN_LIBRARY"), "/y/libvulkan.1.dylib"));
	CHECK(!strcmp(env_of(&p, "MLG_WSI_OUTPUT"), "DP-1"));
	CHECK(!strcmp(env_of(&p, "MLG_WSI_MODE"), "auto"));
	CHECK(!strcmp(env_of(&p, "SDL_VIDEO_MAC_FULLSCREEN_SPACES"), "0"));
	CHECK(!strcmp(p.argv[0], "./quake3e.arm64") && !strcmp(p.argv[1], "+map") &&
	      !strcmp(p.argv[2], "q3dm17"));
	CHECK(!strcmp(setting(&p, "cl_renderer"), "vulkan"));
	CHECK(!strcmp(setting(&p, "r_fullscreen"), "1") && !strcmp(setting(&p, "r_mode"), "-2"));
	CHECK(!strcmp(setting(&p, "r_swapInterval"), "1"));
	/* A point inside the monitor's screen: Quake3e opens on that display. */
	CHECK(!strcmp(setting(&p, "vid_xpos"), "2152") && !strcmp(setting(&p, "vid_ypos"), "60"));
	CHECK(p.argv[p.argc] == NULL);

	/* OpenArena's data: the base path and game directory, read at start. */
	in.basepath = "/Users/someone/Games/OpenArena/openarena-0.8.8";
	in.basegame = "baseoa";
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(!strcmp(setting(&p, "fs_basepath"), "/Users/someone/Games/OpenArena/openarena-0.8.8"));
	CHECK(!strcmp(setting(&p, "fs_basegame"), "baseoa"));
	CHECK(!strcmp(setting(&p, "r_mode"), "-2"));
	in.basegame = "../baseoa";
	CHECK(plan_make(&in, &p, why, sizeof(why)) == -1 && strstr(why, "--basegame"));
	in.basegame = NULL;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0 && !setting(&p, "fs_basegame"));
	in.basepath = NULL;

	/* Windowed: three quarters of the screen, centred on it. */
	in.mode = PLAN_MODE_WINDOWED;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(!strcmp(env_of(&p, "MLG_WSI_MODE"), "windowed"));
	CHECK(!strcmp(setting(&p, "r_fullscreen"), "0") && !strcmp(setting(&p, "r_mode"), "-1"));
	CHECK(!strcmp(setting(&p, "r_customwidth"), "1920") &&
	      !strcmp(setting(&p, "r_customheight"), "1080"));
	CHECK(!strcmp(setting(&p, "vid_xpos"), "1832") && !strcmp(setting(&p, "vid_ypos"), "-120"));

	/* No macOS display for the monitor: no placement; the window size
	 * comes from the output's mode. */
	in.have_display = false;
	in.mode_width = 1920;
	in.mode_height = 1080;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(!setting(&p, "vid_xpos") && !setting(&p, "vid_ypos"));
	CHECK(!strcmp(setting(&p, "r_customwidth"), "1440") &&
	      !strcmp(setting(&p, "r_customheight"), "810"));
	in.mode = PLAN_MODE_FULLSCREEN;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(!strcmp(env_of(&p, "MLG_WSI_MODE"), "fullscreen") && !setting(&p, "vid_xpos"));

	/* Any other program: the environment only. */
	in.quake3 = false;
	in.argc = 1;
	in.argv = vkcube;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == 0);
	CHECK(p.argc == 1 && !strcmp(p.argv[0], "vkcube") && p.argv[1] == NULL);
	CHECK(env_of(&p, "MLG_WSI_OUTPUT") && !setting(&p, "r_mode"));
	in.basegame = "baseoa";
	CHECK(plan_make(&in, &p, why, sizeof(why)) == -1 && strstr(why, "--quake3"));
	in.basegame = NULL;

	/* Refusals. */
	in.connector = "";
	CHECK(plan_make(&in, &p, why, sizeof(why)) == -1 && strstr(why, "connector"));
	in.connector = "DP-1";
	in.argc = 0;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == -1 && strstr(why, "no program"));
	in.argc = 1;
	in.quake3 = true;
	in.mode = PLAN_MODE_WINDOWED;
	in.mode_width = in.mode_height = 0;
	CHECK(plan_make(&in, &p, why, sizeof(why)) == -1 && strstr(why, "size"));

	/* Quake3e's own mode fallback, which mlg-run stops the game for. */
	CHECK(plan_quake3_mode_fallback("Setting r_mode -2 failed, falling back on r_mode 3"));
	CHECK(plan_quake3_mode_fallback("Setting r_mode 6 failed, falling back on r_mode 3\n"));
	CHECK(!plan_quake3_mode_fallback("...setting mode -2: 2560 1440"));

	printf("PASS mlg-run plan: RADV-only loader environment, MLG_WSI_OUTPUT/MODE, SDL on its "
	       "screen, Quake3e placement full screen and windowed, game data (OpenArena), mode fallback, "
	       "with and without a macOS display, refusals\n");
	return 0;
}
