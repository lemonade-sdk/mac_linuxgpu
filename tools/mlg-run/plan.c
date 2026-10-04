/* mlg-run's plan (plan.h). */
#include <stdio.h>
#include <string.h>

#include "plan.h"

int plan_mode_parse(const char *text, enum plan_mode *mode)
{
	if (!strcmp(text, "auto"))
		*mode = PLAN_MODE_AUTO;
	else if (!strcmp(text, "fullscreen"))
		*mode = PLAN_MODE_FULLSCREEN;
	else if (!strcmp(text, "windowed"))
		*mode = PLAN_MODE_WINDOWED;
	else
		return -1;
	return 0;
}

const char *plan_mode_name(enum plan_mode mode)
{
	return mode == PLAN_MODE_FULLSCREEN ? "fullscreen" :
	       mode == PLAN_MODE_WINDOWED ? "windowed" : "auto";
}

static int env(struct plan *p, const char *name, const char *value)
{
	if (p->env_count == PLAN_MAX_ENV || strlen(name) >= sizeof(p->env_name[0]) ||
	    strlen(value) >= sizeof(p->env_value[0]))
		return -1;
	strcpy(p->env_name[p->env_count], name);
	strcpy(p->env_value[p->env_count], value);
	p->env_count++;
	return 0;
}

static int arg(struct plan *p, int *added, const char *fmt, long value, const char *text)
{
	if (p->argc == PLAN_MAX_ARGS || *added == PLAN_MAX_ARGS)
		return -1;
	if (text)
		snprintf(p->args[*added], sizeof(p->args[0]), "%s", text);
	else
		snprintf(p->args[*added], sizeof(p->args[0]), fmt, value);
	p->argv[p->argc++] = p->args[(*added)++];
	return 0;
}

/* Quake3e: "+set name value". */
static int set(struct plan *p, int *added, const char *name, long value, const char *text)
{
	return arg(p, added, NULL, 0, "+set") || arg(p, added, NULL, 0, name) ||
	       arg(p, added, "%ld", value, text);
}

int plan_make(const struct plan_input *in, struct plan *out, char *why, size_t why_size)
{
	int added = 0;

	memset(out, 0, sizeof(*out));
	why[0] = 0;
	if (!in->icd_json || !in->loader || !in->connector || !in->connector[0]) {
		snprintf(why, why_size, "the ICD, the loader and the connector are needed");
		return -1;
	}
	if (in->argc < 1 || !in->argv || !in->argv[0]) {
		snprintf(why, why_size, "no program to run");
		return -1;
	}
	if (in->argc > PLAN_MAX_ARGS - 32) {
		snprintf(why, why_size, "too many arguments (%d)", in->argc);
		return -1;
	}
	/* RADV only: the loader sees no other driver (MoltenVK, KosmicKrisp),
	 * and SDL loads the loader, not a MoltenVK it may bundle. */
	if (env(out, "VK_DRIVER_FILES", in->icd_json) || env(out, "VK_ICD_FILENAMES", in->icd_json) ||
	    env(out, "SDL_VULKAN_LIBRARY", in->loader) ||
	    /* RADV's frames on the output's monitor, by the GPU. */
	    env(out, "MLG_WSI_OUTPUT", in->connector) ||
	    env(out, "MLG_WSI_MODE", plan_mode_name(in->mode)) ||
	    /* SDL's full screen as a borderless window on its screen, not a
	     * Space of its own: the window can be put on the monitor's screen. */
	    env(out, "SDL_VIDEO_MAC_FULLSCREEN_SPACES", "0")) {
		snprintf(why, why_size, "a path is too long");
		return -1;
	}
	for (int i = 0; i < in->argc; i++)
		out->argv[out->argc++] = in->argv[i];

	if (in->quake3) {
		const bool windowed = in->mode == PLAN_MODE_WINDOWED;
		int r = set(out, &added, "cl_renderer", 0, "vulkan") ||
			set(out, &added, "r_swapInterval", 1, NULL) ||
			set(out, &added, "r_fullscreen", windowed ? 0 : 1, NULL);

		if (!windowed) {
			/* The desktop's mode: the monitor's, which no scaling needs. */
			r = r || set(out, &added, "r_mode", -2, NULL);
			if (in->have_display)
				r = r || set(out, &added, "vid_xpos", in->display_x + (long)in->display_width / 4, NULL) ||
				    set(out, &added, "vid_ypos", in->display_y + (long)in->display_height / 4, NULL);
		} else {
			/* Three quarters of the monitor, centred on its screen. */
			uint32_t w = (in->have_display ? in->display_width : in->mode_width) * 3 / 4 & ~1u;
			uint32_t h = (in->have_display ? in->display_height : in->mode_height) * 3 / 4 & ~1u;

			if (!w || !h) {
				snprintf(why, why_size, "no size to make the window from");
				return -1;
			}
			r = r || set(out, &added, "r_mode", -1, NULL) ||
			    set(out, &added, "r_customwidth", w, NULL) ||
			    set(out, &added, "r_customheight", h, NULL);
			if (in->have_display)
				r = r || set(out, &added, "vid_xpos",
					     in->display_x + (long)(in->display_width - w) / 2, NULL) ||
				    set(out, &added, "vid_ypos",
					     in->display_y + (long)(in->display_height - h) / 2, NULL);
		}
		if (r) {
			snprintf(why, why_size, "too many arguments");
			return -1;
		}
	}
	out->argv[out->argc] = NULL;
	return 0;
}
