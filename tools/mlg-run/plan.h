/* mlg-run's plan (plan.c): from what the user asked and what was found
 * (the RADV ICD, the Vulkan loader, the display output and its macOS
 * display), the environment and the command line a program runs with so
 * that it renders on the GPU with RADV and its frames are scanned out by
 * the GPU on the output's monitor. No I/O: mlg-run.c finds the inputs and
 * runs the plan; test_plan.c checks it. */
#ifndef MLG_RUN_PLAN_H
#define MLG_RUN_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLAN_MAX_ENV	16
#define PLAN_MAX_ARGS	256
#define PLAN_TEXT	1024

enum plan_mode {
	PLAN_MODE_AUTO,		/* the window decides (full screen when it fills its screen) */
	PLAN_MODE_FULLSCREEN,
	PLAN_MODE_WINDOWED,
};

struct plan_input {
	const char *icd_json;	/* the RADV ICD manifest */
	const char *loader;	/* the Vulkan loader library (libvulkan.1.dylib) */
	const char *connector;	/* the output's connector ("DP-1") */
	enum plan_mode mode;
	bool quake3;		/* the program is Quake3e: add its placement settings */
	const char *basepath;	/* Quake3e: the directory that holds the game data, or NULL */
	const char *basegame;	/* Quake3e: the game directory in it ("baseoa"), or NULL */
	/* The macOS display that stands for the monitor, if there is one. */
	bool have_display;
	int32_t display_x, display_y;	/* its bounds in global points */
	uint32_t display_width, display_height;
	uint32_t mode_width, mode_height;	/* the output's mode, pixels */
	int argc;		/* the program and its arguments */
	char *const *argv;
};

struct plan {
	char env_name[PLAN_MAX_ENV][64];
	char env_value[PLAN_MAX_ENV][PLAN_TEXT];
	int env_count;
	char *argv[PLAN_MAX_ARGS + 1];	/* NULL-terminated; points into args or the input */
	char args[PLAN_MAX_ARGS][64];	/* arguments the plan adds */
	int argc;
};

/* 0, or -1 with a reason in @why. */
int plan_make(const struct plan_input *in, struct plan *out, char *why, size_t why_size);
/* Whether a line Quake3e printed says it gave up the mode mlg-run set
 * ("Setting r_mode -2 failed, falling back on r_mode 3"). */
bool plan_quake3_mode_fallback(const char *line);

/* "auto", "fullscreen", "windowed" -> 0 with *mode, else -1. */
int plan_mode_parse(const char *text, enum plan_mode *mode);
const char *plan_mode_name(enum plan_mode mode);

#endif
