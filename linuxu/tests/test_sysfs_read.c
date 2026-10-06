/* The in-memory sysfs tree and its reads (linuxu/src/shims/sysfs.c): path
 * walk through kobject directories, named groups, links and the hwmon class
 * directory; show() through each ktype's sysfs_ops into a zeroed page;
 * bin_attribute reads; listing; Linux errnos; and removal draining an
 * in-flight show(), as kernfs does. Also the PCI attributes
 * (drivers/pci/pci-sysfs.c formats) of the runtime PCI device. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <rt/sysfs.h>

const char linuxu_dma_ops = 0;
void *kzalloc(size_t size, gfp_t flags) { (void)flags; return calloc(1, size); }
void kfree(const void *p) { free((void *)p); }
void spin_lock_init(spinlock_t *s) { (void)s; }
void devres_release_all(struct device *d) { (void)d; }
void linuxu_bug(const char *f, int l) { (void)f; (void)l; abort(); }
void linuxu_warn(const char *f, int l, const char *fmt, ...) { (void)f; (void)l; (void)fmt; abort(); }
/* pci-sysfs link attributes read the PCIe capability; offline it is absent. */
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{ (void)dev; (void)pos; *val = 0; return 0; }
/* Configuration space: byte i holds i ^ 0x5a; reads of each width counted. */
static unsigned int config_reads[5];
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{ (void)dev; config_reads[1]++; *val = (u8)(where ^ 0x5a); return 0; }
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{ (void)dev; assert(!(where & 1)); config_reads[2]++; *val = (u16)((u8)(where ^ 0x5a) | (u8)((where + 1) ^ 0x5a) << 8); return 0; }
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
	(void)dev; assert(!(where & 3)); config_reads[4]++;
	*val = 0;
	for (int i = 0; i < 4; i++) *val |= (u32)(u8)((where + i) ^ 0x5a) << (8 * i);
	return 0;
}
enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev) { (void)dev; return PCIE_SPEED_16_0GT; }
enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev) { (void)dev; return PCIE_LNK_WIDTH_X16; }

struct fixture {
	struct device dev;
	int busy;		/* gpu_busy_percent */
	int error;		/* show() error to return */
};
static struct fixture *of(struct device *dev) { return dev_get_drvdata(dev); }

static ssize_t gpu_busy_percent_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	assert(buf[0] == 0 && buf[PAGE_SIZE - 1] == 0); /* a zeroed page */
	return of(dev)->error ? of(dev)->error : sysfs_emit(buf, "%d\n", of(dev)->busy);
}
static DEVICE_ATTR_RO(gpu_busy_percent);
static ssize_t overflow_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev; (void)attr;
	memset(buf, 'x', PAGE_SIZE);
	return PAGE_SIZE;	/* bad count: clamped to PAGE_SIZE - 1 */
}
static DEVICE_ATTR_RO(overflow);
static char secret_value[16];
static ssize_t secret_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	(void)dev; (void)attr;
	if (strlen(buf) != n) return -EINVAL;	/* NUL-terminated, as kernfs gives it */
	if (!strcmp(buf, "refuse")) return -EINVAL;
	snprintf(secret_value, sizeof(secret_value), "%s", buf);
	return n;
}
static DEVICE_ATTR_WO(secret);
static struct device_attribute dev_attr_noshow = { .attr = { .name = "noshow", .mode = 0444 } };

/* A gpu_metrics-like binary blob. */
static unsigned char blob[100];
static ssize_t blob_read(struct file *f, struct kobject *kobj, const struct bin_attribute *attr,
			 char *buf, loff_t pos, size_t count)
{
	(void)f; (void)kobj; (void)attr;
	assert(pos >= 0 && (size_t)pos + count <= sizeof(blob));
	memcpy(buf, blob + pos, count);
	return (ssize_t)count;
}
static const struct bin_attribute bin_attr_blob = {
	.attr = { .name = "blob", .mode = 0444 }, .size = sizeof(blob), .read = blob_read,
};

/* A named group with one invisible file, as amdgpu's is_visible() hides
 * unsupported attributes. */
static ssize_t shown_show(struct device *dev, struct device_attribute *attr, char *buf)
{ (void)dev; (void)attr; return sysfs_emit(buf, "shown\n"); }
static DEVICE_ATTR_RO(shown);
static ssize_t hidden_show(struct device *dev, struct device_attribute *attr, char *buf)
{ (void)dev; (void)attr; abort(); }
static DEVICE_ATTR_RO(hidden);
static struct attribute *power_attrs[] = { &dev_attr_shown.attr, &dev_attr_hidden.attr, NULL };
static umode_t power_visible(struct kobject *kobj, struct attribute *attr, int index)
{ (void)kobj; return index ? 0 : attr->mode; }
static const struct attribute_group power_group = {
	.name = "power", .attrs = power_attrs, .is_visible = power_visible,
};

/* hwmon: a sensor attribute reads its device's driver data. */
static ssize_t temp1_input_show(struct device *dev, struct device_attribute *attr, char *buf)
{ (void)attr; return sysfs_emit(buf, "%d\n", of(dev)->busy * 1000); }
static DEVICE_ATTR_RO(temp1_input);
static struct attribute *sensor_attrs[] = { &dev_attr_temp1_input.attr, NULL };
static const struct attribute_group sensor_group = { .attrs = sensor_attrs };
static const struct attribute_group *sensor_groups[] = { &sensor_group, NULL };

/* A kobject directory with kobj_attributes (kobj_sysfs_ops). */
static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{ (void)kobj; (void)attr; return sysfs_emit(buf, "12.0.1\n"); }
static struct kobj_attribute version_attr = __ATTR_RO(version);

static void release_fixture(struct device *dev) { (void)dev; }

static long rd(struct kobject *root, const char *path, char *buf, size_t n)
{
	memset(buf, 0, n);
	return linuxu_sysfs_read(root, path, buf, n - 1, 0, NULL);
}

static void paths_and_reads(void)
{
	struct fixture f = { .busy = 42 };
	char buf[PAGE_SIZE + 64];
	size_t length = 0;
	device_initialize(&f.dev);
	f.dev.release = release_fixture;
	dev_set_drvdata(&f.dev, &f);
	assert(!dev_set_name(&f.dev, "0000:03:00.0"));
	assert(!device_add(&f.dev));
	struct kobject *dev = &f.dev.kobj;
	assert(!device_create_file(&f.dev, &dev_attr_gpu_busy_percent));
	assert(!device_create_file(&f.dev, &dev_attr_overflow));
	assert(!device_create_file(&f.dev, &dev_attr_secret));
	assert(!device_create_file(&f.dev, &dev_attr_noshow));
	assert(!device_create_bin_file(&f.dev, &bin_attr_blob));
	assert(!sysfs_create_group(dev, &power_group));

	/* show() through dev_sysfs_ops, with the attribute's length. */
	assert(rd(dev, "gpu_busy_percent", buf, sizeof(buf)) == 3 && !strcmp(buf, "42\n"));
	assert(rd(NULL, "0000:03:00.0/gpu_busy_percent", buf, sizeof(buf)) == 3);
	f.busy = 7;
	assert(linuxu_sysfs_read(dev, "gpu_busy_percent", buf, 1, 1, &length) == 1 &&
	       buf[0] == '\n' && length == 2);
	assert(linuxu_sysfs_read(dev, "gpu_busy_percent", buf, 8, 2, &length) == 0 && length == 2);
	f.error = -EBUSY;	/* show()'s own error is the read's */
	assert(rd(dev, "gpu_busy_percent", buf, sizeof(buf)) == -EBUSY);
	f.error = 0;
	assert(linuxu_sysfs_read(dev, "overflow", buf, sizeof(buf), 0, &length) == PAGE_SIZE - 1 &&
	       length == PAGE_SIZE - 1);
	assert(rd(dev, "secret", buf, sizeof(buf)) == -EACCES);
	assert(rd(dev, "noshow", buf, sizeof(buf)) == -EIO);

	/* bin_attribute: clamped to its size, at an offset. */
	for (size_t i = 0; i < sizeof(blob); ++i) blob[i] = (unsigned char)i;
	assert(linuxu_sysfs_read(dev, "blob", buf, sizeof(buf), 0, &length) == sizeof(blob) &&
	       length == sizeof(blob) && !memcmp(buf, blob, sizeof(blob)));
	assert(linuxu_sysfs_read(dev, "blob", buf, 50, 90, NULL) == 10 && buf[0] == 90);
	assert(linuxu_sysfs_read(dev, "blob", buf, 50, 100, NULL) == 0);

	/* Named group: is_visible() decided the files at creation. */
	assert(rd(dev, "power/shown", buf, sizeof(buf)) == 6 && !strcmp(buf, "shown\n"));
	assert(rd(dev, "power/hidden", buf, sizeof(buf)) == -ENOENT);
	assert(rd(dev, "power", buf, sizeof(buf)) == -EISDIR);
	assert(rd(dev, "power/shown/x", buf, sizeof(buf)) == -ENOTDIR);

	/* Bad paths. */
	assert(rd(dev, "missing", buf, sizeof(buf)) == -ENOENT);
	assert(rd(dev, "", buf, sizeof(buf)) == -EISDIR);
	assert(rd(dev, "../gpu_busy_percent", buf, sizeof(buf)) == -EINVAL);
	assert(rd(dev, "power/../gpu_busy_percent", buf, sizeof(buf)) == -EINVAL);
	assert(rd(dev, "./gpu_busy_percent", buf, sizeof(buf)) == -EINVAL);
	assert(rd(dev, "/gpu_busy_percent", buf, sizeof(buf)) == -EINVAL);
	assert(rd(dev, "power//shown", buf, sizeof(buf)) == -EINVAL);
	assert(rd(dev, "power/", buf, sizeof(buf)) == -EINVAL);
	char *longname = malloc(LINUXU_SYSFS_PATH_MAX + 2);
	memset(longname, 'a', 300); longname[300] = 0;
	assert(rd(dev, longname, buf, sizeof(buf)) == -ENAMETOOLONG);
	for (size_t i = 0; i < LINUXU_SYSFS_PATH_MAX + 1; ++i) longname[i] = i % 2 ? '/' : 'a';
	longname[LINUXU_SYSFS_PATH_MAX + 1] = 0;
	assert(rd(dev, longname, buf, sizeof(buf)) == -ENAMETOOLONG);
	free(longname);
	assert(linuxu_sysfs_read(dev, "gpu_busy_percent", buf, 4, -1, NULL) == -EINVAL);

	/* Links are followed; kobject directories with kobj_sysfs_ops. */
	struct kobject *node = kobject_create_and_add("topology", dev);
	assert(node && !sysfs_create_file(node, &version_attr.attr));
	assert(rd(dev, "topology/version", buf, sizeof(buf)) == 7 && !strcmp(buf, "12.0.1\n"));
	assert(!sysfs_create_link(node, dev, "device"));
	assert(rd(dev, "topology/device/topology/device/gpu_busy_percent", buf, sizeof(buf)) == 2);

	/* hwmon class directory, as Linux tools find it. */
	struct device *hw = hwmon_device_register_with_groups(&f.dev, "amdgpu", &f, sensor_groups);
	assert(!IS_ERR(hw));
	assert(rd(dev, "hwmon/hwmon0/name", buf, sizeof(buf)) == 7 && !strcmp(buf, "amdgpu\n"));
	assert(rd(dev, "hwmon/hwmon0/temp1_input", buf, sizeof(buf)) == 5 && !strcmp(buf, "7000\n"));

	/* Listing: sorted, typed, paged. */
	memset(buf, 0, sizeof(buf));
	long n = linuxu_sysfs_list(dev, "", buf, sizeof(buf), 0, &length);
	const char *expect = "f blob\nf gpu_busy_percent\nd hwmon\nf noshow\nf overflow\nd power\n"
			     "f secret\nd topology\n";
	assert(n == (long)strlen(expect) && length == strlen(expect) && !strcmp(buf, expect));
	memset(buf, 0, sizeof(buf));
	assert(linuxu_sysfs_list(dev, "", buf, 8, 7, &length) == 8 && !strcmp(buf, "f gpu_bu"));
	memset(buf, 0, sizeof(buf));
	assert(linuxu_sysfs_list(dev, "topology", buf, sizeof(buf), 0, NULL) == 19 &&
	       !strcmp(buf, "l device\nf version\n"));
	memset(buf, 0, sizeof(buf));
	assert(linuxu_sysfs_list(dev, "power", buf, sizeof(buf), 0, NULL) == 8 && !strcmp(buf, "f shown\n"));
	assert(linuxu_sysfs_list(dev, "gpu_busy_percent", buf, sizeof(buf), 0, NULL) == -ENOTDIR);
	assert(linuxu_sysfs_list(dev, "nothing", buf, sizeof(buf), 0, NULL) == -ENOENT);
	memset(buf, 0, sizeof(buf));
	assert(linuxu_sysfs_list(NULL, "", buf, sizeof(buf), 0, NULL) == 15 && !strcmp(buf, "d 0000:03:00.0\n"));

	/* Teardown removes every entry. */
	hwmon_device_unregister(hw);
	assert(rd(dev, "hwmon", buf, sizeof(buf)) == -ENOENT);
	sysfs_remove_link(node, "device");
	kobject_put(node);
	assert(rd(dev, "topology/version", buf, sizeof(buf)) == -ENOENT);
	device_remove_file(&f.dev, &dev_attr_gpu_busy_percent);
	assert(rd(dev, "gpu_busy_percent", buf, sizeof(buf)) == -ENOENT);
	device_unregister(&f.dev);
	assert(rd(NULL, "0000:03:00.0/overflow", buf, sizeof(buf)) == -ENOENT);
	assert(!linuxu_sysfs_count(NULL));
}

/* Removal waits for a show() in progress, as kernfs_drain does. */
static atomic_int in_show, release_show, removed;
static ssize_t slow_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev; (void)attr;
	atomic_store(&in_show, 1);
	while (!atomic_load(&release_show)) usleep(1000);
	assert(!atomic_load(&removed));
	return sysfs_emit(buf, "done\n");
}
static DEVICE_ATTR_RO(slow);
static void *reader(void *arg)
{
	char buf[16] = {0};
	assert(linuxu_sysfs_read(arg, "slow", buf, sizeof(buf), 0, NULL) == 5 && !strcmp(buf, "done\n"));
	return NULL;
}
static void *remover(void *arg)
{
	struct device *dev = arg;
	device_unregister(dev);
	atomic_store(&removed, 1);
	return NULL;
}
static void drain(void)
{
	struct device *dev = calloc(1, sizeof(*dev));
	device_initialize(dev);
	dev->release = (void (*)(struct device *))free;
	assert(!dev_set_name(dev, "drain") && !device_add(dev));
	assert(!device_create_file(dev, &dev_attr_slow));
	pthread_t r, w;
	assert(!pthread_create(&r, NULL, reader, &dev->kobj));
	while (!atomic_load(&in_show)) usleep(1000);
	assert(!pthread_create(&w, NULL, remover, dev));
	usleep(50000);
	assert(!atomic_load(&removed));	/* still draining */
	char buf[16];
	assert(linuxu_sysfs_read(NULL, "drain/slow", buf, sizeof(buf), 0, NULL) == -ENOENT);
	atomic_store(&release_show, 1);
	assert(!pthread_join(r, NULL) && !pthread_join(w, NULL));
	assert(atomic_load(&removed) && !linuxu_sysfs_count(NULL));
}

static void pci_attributes(void)
{
	struct pci_dev *pdev = calloc(1, sizeof(*pdev));
	char buf[64];
	pdev->vendor = 0x1002; pdev->device = 0x7551;
	pdev->subsystem_vendor = 0x1002; pdev->subsystem_device = 0x0e3b;
	pdev->revision = 0xc0; pdev->class = 0x030000; pdev->is_pcie_device = 1;
	device_initialize(&pdev->dev);
	pdev->dev.release = (void (*)(struct device *))release_fixture;
	pdev->dev.groups = pci_dev_groups;
	assert(!dev_set_name(&pdev->dev, "0000:03:00.0") && !device_add(&pdev->dev));
	struct kobject *k = &pdev->dev.kobj;
	assert(rd(k, "vendor", buf, sizeof(buf)) == 7 && !strcmp(buf, "0x1002\n"));
	assert(rd(k, "device", buf, sizeof(buf)) == 7 && !strcmp(buf, "0x7551\n"));
	assert(rd(k, "subsystem_device", buf, sizeof(buf)) == 7 && !strcmp(buf, "0x0e3b\n"));
	assert(rd(k, "revision", buf, sizeof(buf)) == 5 && !strcmp(buf, "0xc0\n"));
	assert(rd(k, "class", buf, sizeof(buf)) == 9 && !strcmp(buf, "0x030000\n"));
	assert(rd(k, "max_link_speed", buf, sizeof(buf)) == 15 && !strcmp(buf, "16.0 GT/s PCIe\n"));
	assert(rd(k, "max_link_width", buf, sizeof(buf)) == 3 && !strcmp(buf, "16\n"));
	/* No capability offline: link status reads as zero, like a down link. */
	assert(rd(k, "current_link_speed", buf, sizeof(buf)) == 8 && !strcmp(buf, "Unknown\n"));
	assert(rd(k, "current_link_width", buf, sizeof(buf)) == 2 && !strcmp(buf, "0\n"));
	/* config: the configuration space, cfg_size bytes, aligned reads of
	 * each width, as upstream's pci_read_config. */
	{
		static unsigned char space[4200];
		pdev->cfg_size = 4096;
		assert(linuxu_sysfs_read(k, "config", space, sizeof(space), 0, NULL) == 4096);
		for (int i = 0; i < 4096; i++) assert(space[i] == (unsigned char)(i ^ 0x5a));
		memset(config_reads, 0, sizeof(config_reads));
		assert(linuxu_sysfs_read(k, "config", space, 9, 0x41, NULL) == 9);	/* 0x41..0x49 */
		for (int i = 0; i < 9; i++) assert(space[i] == (unsigned char)((0x41 + i) ^ 0x5a));
		assert(config_reads[1] == 1 && config_reads[2] == 2 && config_reads[4] == 1);	/* byte, word, dword, word */
		assert(linuxu_sysfs_read(k, "config", space, 64, 4090, NULL) == 6);
		assert(linuxu_sysfs_read(k, "config", space, 64, 4096, NULL) == 0);
		pdev->cfg_size = 256;
		assert(linuxu_sysfs_read(k, "config", space, sizeof(space), 0, NULL) == 256);
	}
	device_unregister(&pdev->dev);
	free(pdev);
	assert(!linuxu_sysfs_count(NULL));
}

/* Writes: store() with a NUL-terminated page; its errno comes back. */
static void writes(void)
{
	static struct fixture fx;
	struct device *dev = &fx.dev;

	device_initialize(dev);
	dev->release = release_fixture;
	assert(!dev_set_name(dev, "card1"));
	assert(!device_add(dev));
	assert(!device_create_file(dev, &dev_attr_secret) &&
	       !device_create_file(dev, &dev_attr_gpu_busy_percent));
	assert(linuxu_sysfs_write(&dev->kobj, "secret", "high", 4) == 4 && !strcmp(secret_value, "high"));
	assert(linuxu_sysfs_write(&dev->kobj, "secret", "refuse", 6) == -EINVAL);
	assert(linuxu_sysfs_write(&dev->kobj, "gpu_busy_percent", "1", 1) == -EACCES);	/* read-only */
	assert(linuxu_sysfs_write(&dev->kobj, "missing", "1", 1) == -ENOENT);
	assert(linuxu_sysfs_write(&dev->kobj, "secret", "", 0) == -EINVAL);
	device_remove_file(dev, &dev_attr_secret);
	device_remove_file(dev, &dev_attr_gpu_busy_percent);
	device_del(dev);
	put_device(dev);
}

int main(void)
{
	writes();
	paths_and_reads();
	drain();
	pci_attributes();
	puts("PASS sysfs: writes through store(), path walk, groups, links, hwmon class directory, show/bin read, errnos, listing, removal drain, PCI attributes and configuration space");
	return 0;
}
