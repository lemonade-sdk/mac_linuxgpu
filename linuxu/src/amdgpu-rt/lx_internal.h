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

#endif
