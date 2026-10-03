/* linuxu: SHIM (third_party/linux/include/linux/fb.h) */
#ifndef __LINUX_FB_H
#define __LINUX_FB_H
#include <linux/types.h>
/* framebuffer constants (vendor uapi/linux/fb.h values) */
#define FB_TYPE_PACKED_PIXELS		0
#define FB_TYPE_RGB_RGBX		4
#define FB_TYPE_GRABBER			1
#define FB_VISUAL_MONO01		0
#define FB_VISUAL_MONO10		1
#define FB_VISUAL_TRUECOLOR		2
#define FB_VISUAL_PSEUDOCOLOR		3
#define FB_VISUAL_DIRECTCOLOR		4
#define FB_VISUAL_STATIC_PSEUDOCOLOR	5
#define FB_VISUAL_FOURCC		6
#define FB_ACCEL_NONE			0

#define KHZ2PICOS(khz) ((1000000000LL) / (khz) * 1000)

/* struct fb_bitfield (vendor uapi/linux/fb.h) */
struct fb_bitfield {
	u32 offset;		/* first bit of subfield */
	u32 length;		/* number of bits */
	u32 msb_right;		/* subfield grows rt to left */
};

/* struct fb_var_screeninfo — vendor uapi/linux/fb.h layout (bitfield form) */
struct fb_var_screeninfo {
	u32 xres;
	u32 yres;
	u32 xres_virtual;
	u32 yres_virtual;
	u32 xoffset;
	u32 yoffset;

	u32 bits_per_pixel;
	u32 grayscale;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;

	u32 nonstd;
	u32 activate;
	u32 gamma;
	u32 height;
	u32 width;
	u32 accel_flags;
	u32 pixclock;
	u32 left_margin;
	u32 right_margin;
	u32 upper_margin;
	u32 lower_margin;
	u32 hsync_len;
	u32 vsync_len;
	u32 sync;
	u32 vmode;
	u32 rotate;
	u32 colorspace;
	u32 reserved[4];
};

struct fb_fix_screeninfo {
	u32 smem_start;
	u32 smem_len;
	u8 type;
	u8 type_aux;
	u16 visual;
	u16 xpanstep;
	u16 ypanstep;
	u16 ywrapstep;
	u16 line_length;
	u32 mmio_start;
	u32 mmio_len;
	u8 *smem_start_virtual;
	u16 io_width;
	char id[16];
	u16 reserved;
	u8 accel;
	u32 reserved2[7];
};

/* struct fb_cmap — verbatim from vendor uapi/linux/fb.h */
struct fb_cmap {
	u32 start;			/* First entry */
	u32 len;			/* Number of entries */
	u16 *red;			/* Red values */
	u16 *green;
	u16 *blue;
	u16 *transp;			/* transparency, can be NULL */
};

struct fb_info {
	struct model_blob *model;
	unsigned int node;
	u32 flags;
	int state;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	void *fbops;
	void *par;
	struct module *fbdev;
	const char *fix_name;
	int screen_size;
	struct fb_cmap cmap;
	u32 *pseudo_palette;
	bool skip_vt_switch;
	bool skip_panic;
	int fbcon_rotate_hint;
};


#define FB_ACTIVATE_DEFAULT		0
#define FB_ACTIVATE_NOW			1
#define FB_ACTIVATE_KD_TEXT		2
#define FB_ACTIVATE_NEXT		2
#define FB_ACTIVATE_ONCE		4
#define FB_ACTIVATE_DEFER			8
#define FB_ROTATE_UR			0
#define FB_ROTATE_CW			90
#define FB_ROTATE_UD			180
#define FB_ROTATE_CCW			270
#define FBIO_WAITFORVSYNC			_IOW('FB', 0x20, __kernel_caddr_t)
#define FB_BLANK_UNBLANK		0
#define FB_BLANK_NORMAL	1
#define FB_BLANK_VSYNC	2
#define FB_BLANK_HSYNC	3
#define FB_BLANK_POWERDOWN	4
#define FB_BLANK_HSYNC_SUSPEND	5
#define FB_BLANK_VSYNC_SUSPEND	6
#define FB_BLANK_HSYNC_VSYNC_SUSPEND 7
#define FBINFO_HIDE_SMEM_START 0x100
#define FBINFO_MODULE 0x200
#define FBINFO_STATE_RUNNING 0
#define FBINFO_STATE_SUSPENDED 1
#define FB_BLANK_HSYNC_SUSPEND	5
#define FB_BLANK_VSYNC_SUSPEND	6
#define FB_BLANK_HSYNC_VSYNC_SUSPEND 7

static inline int fb_set_suspend(struct fb_info *info, int suspend)
{
	(void)info; (void)suspend;
	return 0;
}

extern void *framebuffer_alloc(unsigned long size, void *bus);
extern int register_framebuffer(struct fb_info *info);
extern void unregister_framebuffer(struct fb_info *info);
extern void framebuffer_release(struct fb_info *info);

static inline int fb_alloc_cmap(struct fb_cmap *cmap, int len, int sizes)
{
	(void)cmap; (void)len; (void)sizes;
	return 0;
}
static inline void fb_dealloc_cmap(struct fb_cmap *cmap)
{
	(void)cmap;
}
#endif
