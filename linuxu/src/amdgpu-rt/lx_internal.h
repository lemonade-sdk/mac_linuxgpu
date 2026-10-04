/* Linux-file clients: the pieces of lx_files.c that know a device's memory
 * (lx_gem.c). Internal to linuxu/src/amdgpu-rt. */
#ifndef LINUXU_LX_INTERNAL_H
#define LINUXU_LX_INTERNAL_H

#include <stdint.h>

struct drm_device;
struct pci_dev;
struct vm_area_struct;

/* The BAR of @pdev holding bus range [@bus, @bus + @bytes): its index and
 * the offset in it. -ERANGE when no BAR holds the whole range. */
int rt_lx_bar_of(struct pci_dev *pdev, uint64_t bus, uint64_t bytes, uint32_t *bar,
		 uint64_t *offset);

/* A render-node VMA that ->mmap set up for a GEM object (drm_gem_mmap):
 * make its first @length bytes CPU reachable as the Linux fault path would
 * (a VRAM buffer moves to CPU-visible VRAM), pin it there, and report its
 * backing through @add (RT_LX_RANGE_CPU page runs or RT_LX_RANGE_BAR
 * ranges, in order). *@pinned is what rt_lx_gem_unpin releases. */
int rt_lx_gem_map(struct drm_device *ddev, struct pci_dev *pdev,
		  struct vm_area_struct *vma, uint64_t length, void **pinned,
		  uint32_t *backing, uint32_t *cache,
		  int (*add)(void *arg, uint32_t bar, uint64_t addr, uint64_t bytes),
		  void *arg);
void rt_lx_gem_unpin(void *pinned);

/* What a client's primary-node files and LX_SCANOUT need from the display
 * (display.c's rt_display_lx_hooks; rt_lx_client_set_display). */
struct file;
struct mlg_lx_scanout;
struct mlg_lx_scanout_state;
struct rt_lx_display_hooks {
	/* Around opening a primary-node file: no modeset of the driver's
	 * own may run while the new file is briefly DRM master. */
	void (*primary_lock)(void);
	void (*primary_unlock)(void);
	/* Inside the process, the file just opened: make it a client that
	 * is not DRM master. 0 or -errno (the file is then closed). */
	int (*primary_opened)(struct drm_device *ddev, struct file *file);
	/* LX_SCANOUT inside the client's process; @file is the client's
	 * primary-node file @req->fd names, for TEST and PRESENT (else NULL);
	 * @owner identifies the client. */
	int (*scanout)(struct pci_dev *pdev, void *owner, struct file *file,
		       const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state);
	/* The client is going away, before its files close: take its
	 * framebuffers off the screen. */
	void (*client_gone)(struct pci_dev *pdev, void *owner);
};

#endif
