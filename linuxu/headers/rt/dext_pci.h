#ifndef LINUXU_RT_DEXT_PCI_H
#define LINUXU_RT_DEXT_PCI_H

#include <stdint.h>

/* DriverKit PCI data copied into the Linux compatibility device after the
 * IOPCIDevice session is open.  BAR bases are the assigned PCI addresses;
 * memory_index is the separate DriverKit MemoryRead/Write selector. */
struct dext_pci_bar {
	uint64_t base;
	uint64_t size;
	uint8_t memory_index;
	uint8_t type;
	uint8_t present;
	uint8_t reserved[5];
};

struct dext_pci_snapshot {
	uint16_t vendor;
	uint16_t device;
	uint16_t subsystem_vendor;
	uint16_t subsystem_device;
	uint32_t class_code;
	uint8_t revision;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	struct dext_pci_bar bar[6];
};

#ifdef __cplusplus
extern "C" {
#endif
/* Cold/idle function reset only. Live DMA or IRQ ownership returns EBUSY. */
int dext_pci_function_reset(void);
/* The shutdown owner must first retire compute and upstream resources under
 * dext_dma_begin_shutdown(), then drain platform IRQs. Reset leaves BM off
 * and only releases retired DART mappings after verification succeeds. */
int dext_pci_shutdown_reset(void);
/* Permanently reject new seam accesses, drain admitted calls, and disable BM
 * without resetting or invalidating provider/token ownership. */
int dext_pci_quarantine(void);
/* Surprise removal. dext_pci_device_present asks the provider's
 * configuration space directly (1 when it answers, or when no provider is
 * open). dext_pci_mark_removed closes admission for good: nothing touches
 * the device again and nothing resets or isolates it. dext_pci_close_removed
 * closes the provider once no access is in flight and no interrupt source
 * is left, then reopens admission and clears the fault records for the
 * next device (0; -16 while something remains; -22 if not removed). */
int dext_pci_device_present(void);
void dext_pci_mark_removed(void);
int dext_pci_removed(void);
int dext_pci_close_removed(void);
/* 1 when only dext_pci_quarantine() closed admission and nothing is admitted,
 * resetting or interrupt-owned (cached state). */
int dext_pci_quarantine_releasable(void);
/* Reopen admission closed by dext_pci_quarantine() so the owner can run its
 * verified shutdown reset. The owner must already have proven quiescence.
 * 0 on success; -5 after a definite transport fault; -16 while busy. */
int dext_pci_release_quarantine(void);
int dext_pci_snapshot(struct dext_pci_snapshot *snapshot);
int dext_bar_info(uint8_t bar, uint8_t *memory_index, uint64_t *size);
int dext_pci_config_read8(uint64_t offset, uint8_t *value);
int dext_pci_config_read16(uint64_t offset, uint16_t *value);
int dext_pci_config_read32(uint64_t offset, uint32_t *value);
int dext_pci_config_write8(uint64_t offset, uint8_t value);
int dext_pci_config_write16(uint64_t offset, uint16_t value);
int dext_pci_config_write32(uint64_t offset, uint32_t value);
/* A dword of BAR @bar (a BAR register index, 0-5) at @offset, through the
 * kernel's MemoryRead32/MemoryWrite32 (the MSI-X table). 0 or -1. */
int dext_pci_bar_read32(unsigned int bar, uint64_t offset, uint32_t *value);
int dext_pci_bar_write32(unsigned int bar, uint64_t offset, uint32_t value);
/* First definite local transport failure for the current PCI session.
 * DriverKit accessors return void, so write completion cannot be confirmed
 * through their return value.  All-ones reads are recorded separately: the
 * same bit pattern can be valid register data. */
enum dext_pci_transport_fault {
	DEXT_PCI_FAULT_NONE = 0,
	DEXT_PCI_FAULT_CONFIG = 1,
	DEXT_PCI_FAULT_MMIO = 2,
	DEXT_PCI_FAULT_SNAPSHOT = 3,
	/* Deliberate isolation by dext_pci_quarantine(); offset 0. Recorded
	 * only when no earlier fault exists, so a real first fault survives. */
	DEXT_PCI_FAULT_QUARANTINE = 4,
	/* linuxu_fatal() (BUG, panic, impossible platform failure) contained
	 * the device before parking its thread; offset is the source line. */
	DEXT_PCI_FAULT_FATAL = 5,
};
int dext_pci_transport_fault(void);
uint64_t dext_pci_transport_fault_offset(void);
/* Recording a definite fault also closes PCI admission and quarantines DMA
 * backing before returning. Already admitted operations cannot be cancelled.
 * Sentinel observations alone do not imply failure or close admission. */
void dext_pci_transport_record_fault(int fault, uint64_t offset);
/* Called once, on the thread that recorded the first definite fault (other
 * than FATAL), after admission closed: the session layer's reaction. It may
 * run with any lock held. */
void dext_pci_set_fault_hook(void (*hook)(int fault));
enum dext_pci_transport_sentinel {
	DEXT_PCI_SENTINEL_NONE = 0,
	DEXT_PCI_SENTINEL_CONFIG = 1,
	DEXT_PCI_SENTINEL_MMIO = 2,
};
int dext_pci_transport_sentinel(void);
uint64_t dext_pci_transport_sentinel_offset(void);
void dext_pci_transport_note_sentinel(int source, uint64_t offset);
enum dext_pci_irq_type {
	DEXT_PCI_IRQ_NONE = 0,
	DEXT_PCI_IRQ_MSI = 1,
	DEXT_PCI_IRQ_MSIX = 2,
};
/* Only sources with a bound action and successful SetEnable are counted. */
int dext_pci_irq_status(unsigned int *armed_vectors, unsigned int *type);
#ifdef __cplusplus
}
#endif

#endif
