/* drivers/pci/pci-sysfs.c identity and link attributes, the groups every
 * PCI device gets at device_add, and drivers/pci/probe.c pci_speed_string.
 * The values are the endpoint's own configuration registers; through
 * Thunderbolt the link status is the endpoint's link to the enclosure's
 * upstream port, not the whole path. */
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/sysfs.h>

/* drivers/pci/probe.c pcie_link_speed: LNKSTA/LNKCAP speed encodings. */
static const unsigned char pcie_link_speed[16] = {
	PCI_SPEED_UNKNOWN, PCIE_SPEED_2_5GT, PCIE_SPEED_5_0GT, PCIE_SPEED_8_0GT,
	PCIE_SPEED_16_0GT, PCIE_SPEED_32_0GT, PCIE_SPEED_64_0GT,
};

/* pci_speed_string, for this tree's PCIe speed values. */
const char *pci_speed_string(enum pci_bus_speed speed)
{
	switch (speed) {
	case PCIE_SPEED_2_5GT: return "2.5 GT/s PCIe";
	case PCIE_SPEED_5_0GT: return "5.0 GT/s PCIe";
	case PCIE_SPEED_8_0GT: return "8.0 GT/s PCIe";
	case PCIE_SPEED_16_0GT: return "16.0 GT/s PCIe";
	case PCIE_SPEED_32_0GT: return "32.0 GT/s PCIe";
	case PCIE_SPEED_64_0GT: return "64.0 GT/s PCIe";
	default: return "Unknown";
	}
}

#define pci_config_attr(field, format)					\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	(void)attr;							\
	return sysfs_emit(buf, format, to_pci_dev(dev)->field);		\
}									\
static struct device_attribute dev_attr_##field = __ATTR_RO(field)
pci_config_attr(vendor, "0x%04x\n");
pci_config_attr(device, "0x%04x\n");
pci_config_attr(subsystem_vendor, "0x%04x\n");
pci_config_attr(subsystem_device, "0x%04x\n");
pci_config_attr(revision, "0x%02x\n");
pci_config_attr(class, "0x%06x\n");

static ssize_t max_link_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return sysfs_emit(buf, "%s\n", pci_speed_string(pcie_get_speed_cap(to_pci_dev(dev))));
}
static struct device_attribute dev_attr_max_link_speed = __ATTR_RO(max_link_speed);

static ssize_t max_link_width_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return sysfs_emit(buf, "%u\n", (unsigned)pcie_get_width_cap(to_pci_dev(dev)));
}
static struct device_attribute dev_attr_max_link_width = __ATTR_RO(max_link_width);

static ssize_t current_link_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u16 status;
	(void)attr;
	if (pcie_capability_read_word(to_pci_dev(dev), PCI_EXP_LNKSTA, &status))
		return -EINVAL;
	return sysfs_emit(buf, "%s\n", pci_speed_string(pcie_link_speed[status & 0x000f]));
}
static struct device_attribute dev_attr_current_link_speed = __ATTR_RO(current_link_speed);

static ssize_t current_link_width_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u16 status;
	(void)attr;
	if (pcie_capability_read_word(to_pci_dev(dev), PCI_EXP_LNKSTA, &status))
		return -EINVAL;
	return sysfs_emit(buf, "%u\n", (unsigned)((status & 0x03f0) >> 4));
}
static struct device_attribute dev_attr_current_link_width = __ATTR_RO(current_link_width);

static struct attribute *pci_dev_attrs[] = {
	&dev_attr_vendor.attr, &dev_attr_device.attr,
	&dev_attr_subsystem_vendor.attr, &dev_attr_subsystem_device.attr,
	&dev_attr_revision.attr, &dev_attr_class.attr, NULL,
};
static const struct attribute_group pci_dev_group = { .attrs = pci_dev_attrs };

static struct attribute *pcie_dev_attrs[] = {
	&dev_attr_current_link_speed.attr, &dev_attr_current_link_width.attr,
	&dev_attr_max_link_width.attr, &dev_attr_max_link_speed.attr, NULL,
};
static umode_t pcie_dev_attrs_are_visible(struct kobject *kobj, struct attribute *a, int n)
{
	(void)n;
	return pci_is_pcie(to_pci_dev(container_of(kobj, struct device, kobj))) ? a->mode : 0;
}
static const struct attribute_group pcie_dev_attr_group = {
	.attrs = pcie_dev_attrs,
	.is_visible = pcie_dev_attrs_are_visible,
};

/* drivers/pci/pci-sysfs.c pci_read_config: the "config" file, the device's
 * configuration space as lspci and setpci read it: a byte, a word, then
 * whole dwords, then a word and a byte, each little endian, through the
 * driver's configuration reads. Read-only here (upstream also writes). */
static ssize_t config_read(struct file *filp, struct kobject *kobj, const struct bin_attribute *bin_attr,
			   char *buf, loff_t off, size_t count)
{
	struct pci_dev *dev = to_pci_dev(container_of(kobj, struct device, kobj));
	unsigned int size = dev->cfg_size ? (unsigned int)dev->cfg_size : 256;
	loff_t init_off = off;
	u8 *data = (u8 *)buf;

	(void)filp; (void)bin_attr;
	if (off > size)
		return 0;
	if (off + count > size)
		count = size - off;
	size = (unsigned int)count;
	if ((off & 1) && size) {
		u8 val;

		pci_read_config_byte(dev, (int)off, &val);
		data[off - init_off] = val;
		off++;
		size--;
	}
	if ((off & 3) && size > 2) {
		u16 val;

		pci_read_config_word(dev, (int)off, &val);
		data[off - init_off] = val & 0xff;
		data[off - init_off + 1] = (val >> 8) & 0xff;
		off += 2;
		size -= 2;
	}
	while (size > 3) {
		u32 val;

		pci_read_config_dword(dev, (int)off, &val);
		data[off - init_off] = val & 0xff;
		data[off - init_off + 1] = (val >> 8) & 0xff;
		data[off - init_off + 2] = (val >> 16) & 0xff;
		data[off - init_off + 3] = (val >> 24) & 0xff;
		off += 4;
		size -= 4;
	}
	if (size >= 2) {
		u16 val;

		pci_read_config_word(dev, (int)off, &val);
		data[off - init_off] = val & 0xff;
		data[off - init_off + 1] = (val >> 8) & 0xff;
		off += 2;
		size -= 2;
	}
	if (size > 0) {
		u8 val;

		pci_read_config_byte(dev, (int)off, &val);
		data[off - init_off] = val;
	}
	return (ssize_t)count;
}
static const struct bin_attribute bin_attr_config = {
	.attr = { .name = "config", .mode = 0444 }, .size = 4096, .read = config_read,
};
static const struct bin_attribute *const pci_dev_config_attrs[] = { &bin_attr_config, NULL };
static const struct attribute_group pci_dev_config_attr_group = {
	.bin_attrs = pci_dev_config_attrs,
};

/* pci_bus_type.dev_groups upstream; the runtime PCI device carries them. */
const struct attribute_group *pci_dev_groups[] = {
	&pci_dev_group, &pcie_dev_attr_group, &pci_dev_config_attr_group, NULL,
};
