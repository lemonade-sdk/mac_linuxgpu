/* linuxu: EDITED (third_party/linux/include/linux/firmware.h) - upstream
 * IS_REACHABLE(CONFIG_FW_LOADER) branch kept as real externs
 * (implemented in linuxu/src/fw over the embedded rodata firmware
 * table); cleanup.h DEFINE_FREE dropped, FW_UPLOAD branch dropped. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FIRMWARE_H
#define _LINUX_FIRMWARE_H

#include <linux/types.h>
#include <linux/gfp.h>

#define FW_ACTION_NOUEVENT 0
#define FW_ACTION_UEVENT 1

struct firmware {
	size_t size;
	const u8 *data;

	/* firmware loader private fields */
	void *priv;
};

/**
 * enum fw_upload_err - firmware upload error codes
 * @FW_UPLOAD_ERR_NONE: returned to indicate success
 * @FW_UPLOAD_ERR_HW_ERROR: error signalled by hardware, see kernel log
 * @FW_UPLOAD_ERR_TIMEOUT: SW timed out on handshake with HW/firmware
 * @FW_UPLOAD_ERR_CANCELED: upload was cancelled by the user
 * @FW_UPLOAD_ERR_BUSY: there is an upload operation already in progress
 * @FW_UPLOAD_ERR_INVALID_SIZE: invalid firmware image size
 * @FW_UPLOAD_ERR_RW_ERROR: read or write to HW failed, see kernel log
 * @FW_UPLOAD_ERR_WEAROUT: FLASH device is approaching wear-out, wait & retry
 * @FW_UPLOAD_ERR_FW_INVALID: invalid firmware file
 * @FW_UPLOAD_ERR_MAX: Maximum error code marker
 */
enum fw_upload_err {
	FW_UPLOAD_ERR_NONE,
	FW_UPLOAD_ERR_HW_ERROR,
	FW_UPLOAD_ERR_TIMEOUT,
	FW_UPLOAD_ERR_CANCELED,
	FW_UPLOAD_ERR_BUSY,
	FW_UPLOAD_ERR_INVALID_SIZE,
	FW_UPLOAD_ERR_RW_ERROR,
	FW_UPLOAD_ERR_WEAROUT,
	FW_UPLOAD_ERR_FW_INVALID,
	FW_UPLOAD_ERR_MAX
};

struct fw_upload {
	void *dd_handle; /* reference to parent driver */
	void *priv;	 /* firmware loader private fields */
};

/**
 * struct fw_upload_ops - device specific operations to support firmware upload
 * @prepare:		  Required: Prepare secure update
 * @write:		  Required: The write() op receives the remaining
 *			  size to be written and must return the actual
 *			  size written or a negative error code. The write()
 *			  op will be called repeatedly until all data is
 *			  written.
 * @poll_complete:	  Required: Check for the completion of the
 *			  HW authentication/programming process.
 * @cancel:		  Required: Request cancellation of update. This op
 *			  is called from the context of a different kernel
 *			  thread, so race conditions need to be considered.
 * @cleanup:		  Optional: Complements the prepare()
 *			  function and is called at the completion
 *			  of the update, on success or failure, if the
 *			  prepare function succeeded.
 */
struct fw_upload_ops {
	enum fw_upload_err (*prepare)(struct fw_upload *fw_upload,
				      const u8 *data, u32 size);
	enum fw_upload_err (*write)(struct fw_upload *fw_upload,
				    const u8 *data, u32 offset,
				    u32 size, u32 *written);
	enum fw_upload_err (*poll_complete)(struct fw_upload *fw_upload);
	void (*cancel)(struct fw_upload *fw_upload);
	void (*cleanup)(struct fw_upload *fw_upload);
};

struct module;
struct device;

/*
 * Firmware loading in the shim resolves names against the embedded
 * rodata firmware table (plan section 2.4, WP W7):
 *   request_firmware()        copies the blob into a kmalloc'd data buffer
 *   request_firmware_direct()  no copy; data points into the rodata table
 *                             (release_firmware() does not free it)
 *   unknown name -> -ENOENT with *fw left NULL
 */
bool firmware_request_builtin(struct firmware *fw, const char *name);

int request_firmware(const struct firmware **fw, const char *name,
		     struct device *device);
int firmware_request_nowait_nowarn(
	struct module *module, const char *name,
	struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context));
int firmware_request_nowarn(const struct firmware **fw, const char *name,
			    struct device *device);
int firmware_request_platform(const struct firmware **fw, const char *name,
			      struct device *device);
int request_firmware_nowait(
	struct module *module, bool uevent,
	const char *name, struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context));
int request_firmware_direct(const struct firmware **fw, const char *name,
			    struct device *device);
int request_firmware_into_buf(const struct firmware **firmware_p,
	const char *name, struct device *device, void *buf, size_t size);
int request_partial_firmware_into_buf(const struct firmware **firmware_p,
				      const char *name, struct device *device,
				      void *buf, size_t size, size_t offset);

void release_firmware(const struct firmware *fw);

int firmware_request_cache(struct device *device, const char *name);


#endif
