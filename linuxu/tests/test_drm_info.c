/* The AMDGPU_INFO reader (linuxu/src/amdgpu-rt/drm_info.c) on the linuxu
 * process substrate: the render node opened into the reader's own process,
 * DRM_IOCTL_AMDGPU_INFO with the request block and return_pointer in that
 * process's user memory, the allow-list, errnos, and close running the
 * file's release. A fixture render node stands in for drm_ioctl and
 * amdgpu_info_ioctl, which copy the same way. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/chardev.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/amdgpu_drm.h>
#include <rt/drm_info.h>

static unsigned int opens, releases, ioctls;
static struct task_struct *opener;

static int render_open(struct inode *inode, struct file *filp)
{
	assert(MAJOR(inode->i_rdev) == DRM_MAJOR && iminor(inode) == 128);
	opener = current->group_leader;
	filp->private_data = &opens;
	++opens;
	return 0;
}
static int render_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	assert(filp->private_data == &opens);
	++releases;
	return 0;
}
/* As drm_ioctl + amdgpu_info_ioctl: copy the request in, answer through
 * return_pointer with at most return_size bytes. */
static long render_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct drm_amdgpu_info info;
	uint32_t words[4] = {0};
	uint32_t n = 0;

	(void)filp;
	++ioctls;
	assert(current->group_leader == opener);	/* the reader process */
	if (cmd != DRM_IOCTL_AMDGPU_INFO)
		return -ENOTTY;
	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;
	if (!info.return_size || !info.return_pointer)
		return -EINVAL;
	switch (info.query) {
	case AMDGPU_INFO_SENSOR:
		if (info.sensor_info.type != AMDGPU_INFO_SENSOR_GPU_LOAD)
			return -EINVAL;
		words[0] = 42;
		n = 4;
		break;
	case AMDGPU_INFO_READ_MMR_REG:
		if (info.read_mmr_reg.count > 4 || info.read_mmr_reg.instance != 0xffffffffu)
			return -EINVAL;
		for (uint32_t i = 0; i < info.read_mmr_reg.count; ++i)
			words[i] = 0x80000000u | (info.read_mmr_reg.dword_offset + i);
		n = info.read_mmr_reg.count * 4;
		break;
	default:
		return -EINVAL;
	}
	if (n > info.return_size)
		n = info.return_size;
	return copy_to_user((void __user *)(uintptr_t)info.return_pointer, words, n) ? -EFAULT : 0;
}
static const struct file_operations render_fops = {
	.open = render_open,
	.release = render_release,
	.unlocked_ioctl = render_ioctl,
};

int main(void)
{
	static struct drm_minor render = { .index = 128 };
	static struct drm_device ddev = { .render = &render };
	static struct pci_dev pdev;
	struct rt_drm_info *info = NULL;
	uint32_t value = 0, regs[4] = {0};

	assert(register_chrdev(DRM_MAJOR, "drm", &render_fops) == 0);
	assert(rt_drm_info_open(&pdev, &info) == -ENODEV && !info && !opens);
	pci_set_drvdata(&pdev, &ddev);
	assert(rt_drm_info_open(&pdev, &info) == 0 && info && opens == 1);
	assert(opener && opener != current->group_leader);

	struct { uint32_t type; } sensor = { AMDGPU_INFO_SENSOR_GPU_LOAD };
	assert(!rt_drm_info_query(info, AMDGPU_INFO_SENSOR, &sensor, sizeof(sensor), &value, 4));
	assert(value == 42);
	sensor.type = AMDGPU_INFO_SENSOR_GFX_SCLK;	/* the ioctl's own errno */
	assert(rt_drm_info_query(info, AMDGPU_INFO_SENSOR, &sensor, sizeof(sensor), &value, 4) == -EINVAL);

	struct { uint32_t dword_offset, count, instance, flags; } mmr = { 0x2004, 3, 0xffffffffu, 0 };
	assert(!rt_drm_info_query(info, AMDGPU_INFO_READ_MMR_REG, &mmr, sizeof(mmr), regs, sizeof(regs)));
	assert(regs[0] == 0x80002004u && regs[2] == 0x80002006u && regs[3] == 0);

	/* Not a monitor query, too large, or malformed: refused before the ioctl. */
	const unsigned int before = ioctls;
	assert(rt_drm_info_query(info, AMDGPU_INFO_FW_VERSION, NULL, 0, &value, 4) == -EPERM);
	assert(rt_drm_info_query(info, AMDGPU_INFO_SENSOR, &sensor, sizeof(sensor), &value,
				 RT_DRM_INFO_MAX_BYTES + 1) == -EINVAL);
	assert(rt_drm_info_query(info, AMDGPU_INFO_SENSOR, &sensor, 17, &value, 4) == -EINVAL);
	assert(rt_drm_info_query(info, AMDGPU_INFO_SENSOR, &sensor, sizeof(sensor), NULL, 4) == -EINVAL);
	assert(ioctls == before);
	assert(rt_drm_info_query_allowed(AMDGPU_INFO_VRAM_GTT) && rt_drm_info_query_allowed(AMDGPU_INFO_MEMORY) &&
	       !rt_drm_info_query_allowed(AMDGPU_INFO_VBIOS));

	rt_drm_info_close(info);
	assert(releases == 1);
	unregister_chrdev(DRM_MAJOR, "drm");
	puts("PASS drm info reader: render node in its own process, INFO request and result in user memory, allow-list, close");
	return 0;
}
