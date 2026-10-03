/* linuxu: SHIM (third_party/linux/include/linux/dev_printk.h)
 * dev_* logging goes through dev_printk() in linux/device.h. */
#ifndef _DEVICE_PRINTK_H_LINUXU_
#define _DEVICE_PRINTK_H_LINUXU_

#include <linux/compiler.h>
#include <linux/types.h>

#ifndef dev_fmt
#define dev_fmt(fmt) fmt
#ifndef dev_dbg_once
#define dev_dbg_once(dev, fmt, ...) dev_dbg(dev, fmt, ##__VA_ARGS__)
#endif
#endif

struct device;

#define PRINTK_INFO_SUBSYSTEM_LEN	16
#define PRINTK_INFO_DEVICE_LEN		48

struct dev_printk_info {
	char subsystem[PRINTK_INFO_SUBSYSTEM_LEN];
	char device[PRINTK_INFO_DEVICE_LEN];
};

#define dev_printk_index_emit(level, fmt, ...) \
	printk(KERN_DEFAULT "%s %s: " fmt, level, fmt, ##__VA_ARGS__)
#define dev_printk_index_wrap(_p_func, level, dev, fmt, ...) \
	_p_func(level, dev, fmt, ##__VA_ARGS__)


static inline int dev_err_probe(struct device *dev, int err_ret,
				const char *fmt, ...)
{
	(void)dev; (void)fmt;
	return err_ret;
}
#endif /* _DEVICE_PRINTK_H_LINUXU_ */
