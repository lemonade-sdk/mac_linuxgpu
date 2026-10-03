/* A libmlg_drm transport over the in-process Linux-file core
 * (lx_loopback.c), for tests that run a libmlg_drm client against a
 * fixture device. Shared by kernel-side and client-side translation
 * units: no kernel types. */
#ifndef LX_LOOPBACK_H
#define LX_LOOPBACK_H

#include <stdint.h>

struct pci_dev;
struct mlg_transport;

/* Create the client's Linux process on @pdev's GPU and fill @out. */
int lx_loopback_transport(struct pci_dev *pdev, struct mlg_transport *out);
void lx_loopback_exit(void);
unsigned int lx_loopback_async_calls(void);
unsigned int lx_loopback_open_files(void);
/* Map BAR-backed mappings (VRAM through the aperture) as well: @fn returns
 * the host memory behind @bytes at @offset of BAR @bar, or NULL. Without
 * it such mappings fail with ENODEV. Ranges must be host-page aligned. */
void lx_loopback_set_bar_memory(void *(*fn)(uint32_t bar, uint64_t offset, uint64_t bytes));

#endif
