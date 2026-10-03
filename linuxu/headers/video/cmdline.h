/* linuxu: AS-IS (third_party/linux/include/video/cmdline.h) */
#ifndef __VIDEO_CMDLINE_H
#define __VIDEO_CMDLINE_H

const char *video_get_options(const char *name);

#define MAX_OPTIONS 4

struct option {
	unsigned int type;
	unsigned int flags;
	int (*parse)(struct option *opt, char *arg);
};

#endif
