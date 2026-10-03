#include <assert.h>
#include <stdint.h>
#include <rt/rt.h>
static irqreturn_t handler(int irq, void *arg)
{
    (void)irq; (void)arg; return IRQ_HANDLED;
}
int main(void)
{
    int owner, other;
    assert(rt_pci_irq_request(NULL, 7, NULL, 0, "null", &owner) != 0);
    assert(rt_pci_irq_request(NULL, UINT32_MAX, handler, 0, "range", &owner) != 0);
    assert(rt_pci_irq_request(NULL, 7, handler, 0, "owner", &owner) == 0);
    rt_pci_irq_free(NULL, 7, &other);
    assert(rt_pci_irq_request(NULL, 7, handler, 0, "duplicate", &owner) != 0);
    rt_pci_irq_free(NULL, 7, &owner);
    assert(rt_pci_irq_request(NULL, 7, handler, 0, "reuse", &owner) == 0);
    rt_pci_irq_free(NULL, 7, &owner);
    return 0;
}
