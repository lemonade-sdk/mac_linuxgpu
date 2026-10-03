/* linuxu: SHIM (display surface — struct fb_info + the bare minimum
 * framebuffer API the amdgpu fbdev path references; the real fbdev
 * core is not part of the compute dext) */
#ifndef _FB_SHIM_LINUXU_H
#define _FB_SHIM_LINUXU_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>

struct fb_apertures {
	struct resource *system;
	struct resource *video;
};
#include <linux/io.h>

struct device;
struct fb_info;
struct fb_var_screeninfo;
struct fb_fix_screeninfo;
struct fb_display_mode;
struct fb_apertures;
struct fb_pixmap;
struct fb_ops;

/**
 * struct fb_var_screeninfo - variable part of the fb info
 *
 * Upstream (uapi) layout preserved.
 */
struct fb_var_screeninfo {
	u32 xres;
	u32 yres;
	u32 xres_virtual;
	u32 yres_virtual;
	u32 xoffset;
	u32 yoffset;
	u16 bits_per_pixel;
	u16 grayscale;
	struct {
		u16 offset;
		u16 length;
	} red;
	struct {
		u16 offset;
		u16 length;
	} green;
	struct {
		u16 offset;
		u16 length;
	} blue;
	struct {
		u16 offset;
		u16 length;
	} transp;
	u32 nonstd;
	u32 activate;
	u32 height;
	u32 width;
	u16 accelerate;
	u16 pixel_clock;
	u32 left_margin;
	u32 right_margin;
	u32 upper_margin;
	u32 lower_margin;
	u32 hsync_len;
	u32 vsync_len;
	u32 sync;
	u32 vmode;
	u32 rotate;
	u32 colors;
	u32 transfer;
	u32 tft_flags;
	u32 tft_width;
	u32 tft_height;
	u32 tft_dpi;
	u32 ext_data;
};

/**
 * struct fb_fix_screeninfo - fixed part of the fb info
 */
struct fb_fix_screeninfo {
	u32 smem_start;
	u32 smem_len;
	u32 type;
	u32 type_aux;
	u32 visual;
	u16 xpanstep;
	u16 ypanstep;
	u16 ywrapstep;
	u16 line_length;
	u32 accel;
	u16 mmu_start;
	u16 mmu_end;
};

/**
 * struct fb_pixmap - fb pixmap
 */
struct fb_pixmap {
	void *buffer;
	u32 size;
	u32 align;
};

/**
 * struct fb_display_mode - a display mode (fbdev)
 */
struct fb_display_mode {
	char name[64];
	struct fb_var_screeninfo var;
	u32 mode_flags;
	u32 flag;
};

/**
 * struct fb_info - framebuffer device info
 *
 * Minimal shape: amdgpu stores one in struct drm_fb_helper and reads
 * fb->fbops / fb->par; the fbdev core itself is not built.
 */
struct fb_info {
	int node;
	unsigned int flags;
	const struct fb_ops *fbops;
	struct device *device;
	void *par;
	void __iomem *screen_base;
	size_t screen_size;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	struct fb_pixmap pixmap;
	const struct fb_display_mode *mode;
	struct list_head mode_list;
	struct fb_display_mode *modes;
	int mode_count;
	int state;
	int count;
	int usercount;
	const struct fb_display_mode *dmi_check_mode;
	struct mutex lock;
	struct fb_apertures apertures;
	struct device *dev;
	char id[16];
	struct module *owner;
	struct fb_info *fb;
	struct device *fbdev;
	void *dma_buf;
	void *dma_attach;
	void *dma_map;
	size_t dma_size;
	struct mutex lock2;
	int state2;
};

int register_framebuffer(struct fb_info *info);
void unregister_framebuffer(struct fb_info *info);
int fb_set_var(struct fb_info *info, const struct fb_var_screeninfo *var);
struct fb_info *framebuffer_alloc(size_t size, struct device *dev);
void framebuffer_release(struct fb_info *info);

#endif
