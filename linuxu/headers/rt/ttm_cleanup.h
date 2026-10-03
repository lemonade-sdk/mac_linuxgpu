#ifndef LINUXU_RT_TTM_CLEANUP_H
#define LINUXU_RT_TTM_CLEANUP_H
struct pci_dev;
/* Called only after failed AMDGPU probe, before its device resources unwind.
 * 1: proven partial TTM owners and device released; 0: no partial TTM device;
 * negative: ownership was not proven; retain driver data, device resources
 * and the containing DRM/PCI owners while quarantining the session. */
int rt_amdgpu_cleanup_failed_probe(struct pci_dev *pdev);
#endif
