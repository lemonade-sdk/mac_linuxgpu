/* A Linux-file call blocked on a held GPU queue holds nothing but its own
 * worker (blocked_call_check.c), on the CS fixture's device. */
#ifndef BLOCKED_CALL_CHECK_H
#define BLOCKED_CALL_CHECK_H

struct pci_dev;

/* The whole check; leaves the compute engine running. */
void blocked_call_check(struct pci_dev *pdev);
/* For removal_check: a client whose submission blocks on the (already
 * held) compute engine, and, once the device is removed, the check that
 * removal ended it and the client's exit completes. */
void blocked_call_park(struct pci_dev *pdev);
void blocked_call_after_removal(void);
/* A hung compute job under GPU recovery: its queue resets, other work goes
 * on, the guilty context reports it (blocked_call_check.c). */
void queue_reset_check(struct pci_dev *pdev);
/* A hang no queue reset ends: the device wedges (leaves it wedged). */
void wedge_check(struct pci_dev *pdev);
/* The same with device resets allowed: upstream's device reset runs, fails
 * on the fixture, and the device wedges (leaves it wedged). */
void device_reset_check(struct pci_dev *pdev);
/* A device reset that succeeds: the queue hooks run, new work runs. */
void device_reset_recovers_check(struct pci_dev *pdev);

#endif
