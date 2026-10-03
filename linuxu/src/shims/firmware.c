/* linuxu shim: firmware - request_firmware over the in-process firmware
 * cache.
 *
 * The dext build cannot open() arbitrary paths, so the whole API resolves
 * names without the filesystem.  Resolution order for every name, with no
 * device-specific list anywhere:
 *
 *   1. a host-provided entry (LoadFirmware push, or an earlier fetch);
 *   2. an on-demand fetch from the user-space firmware servicer through
 *      the shared mailbox (linuxu/src/fw/fw_mailbox.c), cached on success;
 *   3. the optional embedded table generated from the build's firmware
 *      directory;
 *   4. -ENOENT, exactly like Linux when the file is absent, so upstream's
 *      optional-firmware and fallback logic behaves as it does on Linux.
 *
 * Lifecycle is the real kernel one:
 *
 *   request_firmware(fw, name, dev)   resolve, kzalloc a struct firmware,
 *                                     copy the blob, set fw->size, *fw.
 *                                     -ENOENT if the name cannot be
 *                                     resolved (*fw stays NULL), -ENOMEM
 *                                     on OOM.
 *   request_firmware_direct(...)      immutable embedded rodata may be
 *                                     borrowed; mutable entries receive
 *                                     a snapshot under the table lock.
 *   release_firmware(fw)              free the data (only if copied) + the
 *                                     struct (shim slab, kmemcheck-covered).
 *
 * Snapshot ownership remains valid if an override replaces an entry while
 * a firmware request is still in use.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/printk.h>

#include "../fw/fw_table.h"
#include "../fw/fw_mailbox.h"

/* Marker stored in struct firmware::priv for direct (no-copy) loads:
 * release_firmware() must free the struct but NOT the rodata blob. */
#define FW_PRIV_DIRECT ((void *)0x1)

/* Give the host servicer a chance to supply `name` before the embedded
 * fallback is consulted.  A file the servicer does not have is not an
 * error here: the caller falls through to the embedded table and then to
 * -ENOENT.  Other servicer failures (malformed reply, OOM) are returned. */
static int fw_resolve(const char *name)
{
	uint8_t *blob = NULL;
	size_t size = 0;
	int ret;

	if (fw_table_has_override(name) || !fw_mailbox_servicer_present())
		return 0;
	ret = fw_mailbox_fetch(name, &blob, &size);
	if (ret == -ENOENT || ret == -EINVAL)
		return 0;
	if (ret)
		return ret;
	if (fw_table_adopt(name, blob, size)) {
		free(blob);
		return -ENOMEM;
	}
	return 0;
}

static int fw_load_common(const struct firmware **fw, const char *name,
			  struct device *device, int direct)
{
	struct firmware *out;
	int owned, ret;

	(void)device;
	if (!fw) return -EINVAL;
	*fw = NULL;
	if (!name) return -EINVAL;

	ret = fw_resolve(name);
	if (ret) {
		pr_info("firmware request %s failed: %d\n", name, ret);
		return ret;
	}
	out = kzalloc(sizeof(*out), GFP_KERNEL);
	if (!out) {
		pr_info("firmware request %s failed: %d\n", name, -ENOMEM);
		return -ENOMEM;
	}

	/* An override may replace/free the table entry concurrently. Snapshot
	 * mutable bytes under its lock; only process-lifetime rodata is borrowed. */
	ret = fw_table_snapshot(name, direct, &out->data, &out->size, &owned);
	if (ret) {
		pr_info("firmware request %s failed: %d\n", name, ret);
		kfree(out);
		return ret;
	}
	out->priv = owned ? NULL : FW_PRIV_DIRECT;
	*fw = out;
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name,
		     struct device *device)
{
	return fw_load_common(fw, name, device, 0);
}

/* No-copy variant for the large blobs: the returned data points
 * into the rodata table; the caller must not write it. */
int request_firmware_direct(const struct firmware **fw, const char *name,
			    struct device *device)
{
	return fw_load_common(fw, name, device, 1);
}

int firmware_request_nowarn(const struct firmware **fw,
			    const char *name, struct device *device)
{
	return fw_load_common(fw, name, device, 0);
}

int firmware_request_platform(const struct firmware **fw,
			      const char *name, struct device *device)
{
	return fw_load_common(fw, name, device, 0);
}

bool firmware_request_builtin(struct firmware *fw, const char *name)
{
	const struct firmware *tmp;
	int ret;

	if (!fw)
		return false;
	ret = fw_load_common(&tmp, name, NULL, 0);
	if (ret)
		return false;
	/* transfer ownership of the copied data into the caller's struct;
	 * the tmp shell is then a no-data shell whose release_firmware()
	 * frees only the struct (data already moved to *fw). */
	*fw = *tmp;
	kfree(tmp); /* ownership of the data moved into the caller's struct */
	return true;
}

int firmware_request_nowait_nowarn(
	struct module *module, const char *name,
	struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context))
{
	const struct firmware *fw;
	int ret;

	(void)module; (void)gfp;
	ret = request_firmware(&fw, name, device);
	if (ret)
		return ret;
	if (cont)
		cont(fw, context);
	/* synchronous shim: caller contract is that cont() took ownership;
	 * if no callback was given, we own it and must release. */
	if (!cont)
		release_firmware(fw);
	return 0;
}

int request_firmware_nowait(
	struct module *module, bool uevent,
	const char *name, struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context))
{
	(void)uevent;
	return firmware_request_nowait_nowarn(module, name, device,
					      gfp, context, cont);
}

static int firmware_into_buf(const struct firmware **firmware_p,
		const char *name, void *buf, size_t size, size_t offset, int partial)
{
	if (!firmware_p) return -EINVAL;
	*firmware_p = NULL;
	if (!name || !buf || !size) return -EINVAL;
	int ret = fw_resolve(name);
	if (ret) return ret;
	struct firmware *fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw) return -ENOMEM;
	ret = fw_table_read_buffer(name, buf, size, offset, partial, &fw->size);
	if (ret) { kfree(fw); return ret; }
	fw->data = buf;
	/* Both borrowed rodata and caller buffers must outlive the wrapper. */
	fw->priv = FW_PRIV_DIRECT;
	*firmware_p = fw;
	return 0;
}

int request_firmware_into_buf(const struct firmware **firmware_p,
	const char *name, struct device *device, void *buf, size_t size)
{
	(void)device;
	return firmware_into_buf(firmware_p, name, buf, size, 0, 0);
}

int request_partial_firmware_into_buf(const struct firmware **firmware_p,
				      const char *name,
				      struct device *device,
				      void *buf, size_t size, size_t offset)
{
	(void)device;
	return firmware_into_buf(firmware_p, name, buf, size, offset, 1);
}

void release_firmware(const struct firmware *fw)
{
	if (!fw)
		return;
	if (fw->priv != FW_PRIV_DIRECT && fw->data)
		kfree((void *)fw->data);
	kfree((void *)fw);
}

int firmware_request_cache(struct device *device, const char *name)
{
	const struct fw_entry *entry;
	int ret;

	(void)device;
	if (!name)
		return -EINVAL;
	/* "cache" == the name resolves into the in-process table */
	ret = fw_resolve(name);
	if (ret)
		return ret;
	entry = fw_table_find(name);
	return entry ? 0 : -ENOENT;
}
