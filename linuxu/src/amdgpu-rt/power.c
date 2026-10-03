/* Compute power transitions over upstream amdkfd's PM entry points
 * (rt/power.h). */
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include <rt/power.h>

#include "amdgpu.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"

/* The device whose KFD this module holds suspended (one GPU per driver
 * instance; a second device is refused rather than miscounted). */
static struct amdgpu_device *suspended_adev;

static struct kfd_dev *bound_kfd(struct amdgpu_device *adev)
{
	struct kfd_dev *kfd = adev ? adev->kfd.dev : NULL;

	return kfd && kfd->init_complete && kfd->num_nodes ? kfd : NULL;
}

int rt_power_kfd_report(struct amdgpu_device *adev, struct rt_power_report *out)
{
	struct kfd_dev *kfd = bound_kfd(adev);
	struct rt_power_report report = {0};

	if (!kfd)
		return -ENODEV;
	for (uint32_t i = 0; i < kfd->num_nodes; ++i) {
		struct kfd_node *node = kfd->nodes[i];
		struct device_queue_manager *dqm = node ? node->dqm : NULL;
		struct device_process_node *cur;

		if (!dqm)
			continue;
		report.nodes++;
		dqm_lock(dqm);
		if (dqm->sched_running)
			report.running++;
		list_for_each_entry(cur, &dqm->queues, list) {
			struct qcm_process_device *qpd = cur->qpd;
			struct queue *q;

			report.processes++;
			if (qpd_to_pdd(qpd)->has_reset_queue)
				report.reset_marked++;
			list_for_each_entry(q, &qpd->queues_list, list) {
				report.queues++;
				if (q->properties.is_active)
					report.active++;
				if (q->properties.is_evicted)
					report.evicted++;
			}
		}
		dqm_unlock(dqm);
	}
	if (out)
		*out = report;
	return 0;
}

int rt_power_kfd_suspend(struct amdgpu_device *adev, struct rt_power_report *out)
{
	struct kfd_dev *kfd = bound_kfd(adev);
	struct rt_power_report before, after;

	if (!kfd)
		return -ENODEV;
	if (suspended_adev)
		return suspended_adev == adev ? -EALREADY : -EBUSY;
	rt_power_kfd_report(adev, &before);
	kgd2kfd_suspend(kfd, true);
	suspended_adev = adev;
	rt_power_kfd_report(adev, &after);
	if (out)
		*out = after;
	if (after.reset_marked > before.reset_marked || after.active || after.running) {
		pr_err("amdgpu power: KFD suspend left %u of %u queues mapped, %u of %u "
		       "queue managers running, %u processes marked for reset by MES failures\n",
		       after.active, after.queues, after.running, after.nodes,
		       after.reset_marked - before.reset_marked);
		return -EIO;
	}
	pr_info("amdgpu power: KFD suspended: %u processes, %u queues unmapped through MES\n",
		after.processes, after.queues);
	return 0;
}

int rt_power_kfd_resume(struct amdgpu_device *adev, struct rt_power_report *out)
{
	struct kfd_dev *kfd = bound_kfd(adev);
	int r;

	if (!kfd)
		return -ENODEV;
	if (suspended_adev != adev)
		return -EALREADY;
	r = kgd2kfd_resume(kfd, true);
	suspended_adev = NULL;
	if (out)
		rt_power_kfd_report(adev, out);
	if (r)
		pr_err("amdgpu power: KFD resume failed: %d\n", r);
	else
		pr_info("amdgpu power: KFD resumed\n");
	return r;
}

int rt_power_kfd_suspended(const struct amdgpu_device *adev)
{
	return adev && suspended_adev == adev;
}

static struct amdgpu_device *bound_adev(struct pci_dev *pdev)
{
	struct drm_device *ddev = pdev ? pci_get_drvdata(pdev) : NULL;

	return ddev ? drm_to_adev(ddev) : NULL;
}

int rt_power_quiesce(struct pci_dev *pdev, struct rt_power_report *out)
{
	struct amdgpu_device *adev = bound_adev(pdev);

	return adev ? rt_power_kfd_suspend(adev, out) : -ENODEV;
}

int rt_power_resume(struct pci_dev *pdev, struct rt_power_report *out)
{
	struct amdgpu_device *adev = bound_adev(pdev);

	return adev ? rt_power_kfd_resume(adev, out) : -ENODEV;
}

int rt_power_quiesced(struct pci_dev *pdev)
{
	return rt_power_kfd_suspended(bound_adev(pdev));
}
