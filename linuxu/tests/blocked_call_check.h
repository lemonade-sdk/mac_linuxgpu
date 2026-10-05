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

#endif
