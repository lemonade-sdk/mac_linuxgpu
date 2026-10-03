/* host/fw_mailbox_iokit.c - map the dext's firmware mailbox through an
 * IOKit user-client connection and start a servicer on it. */
#include "fw_mailbox_service.h"
#include "../linuxu/headers/rt/fw_mailbox.h"

#include <errno.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

void mlg_fw_service_set_unmap(struct mlg_fw_service *service,
			      void (*unmap)(struct mlg_fw_service *),
			      uint32_t connection, uint64_t address, uint64_t size);
void mlg_fw_service_get_mapping(const struct mlg_fw_service *service,
				uint32_t *connection, uint64_t *address, uint64_t *size);

static void unmap_connection(struct mlg_fw_service *service)
{
	uint32_t connection = 0;
	uint64_t address = 0, size = 0;

	mlg_fw_service_get_mapping(service, &connection, &address, &size);
	if (address)
		IOConnectUnmapMemory64((io_connect_t)connection,
				       MLG_FW_MAILBOX_MEMORY_TYPE,
				       mach_task_self(), (mach_vm_address_t)address);
}

int mlg_fw_service_start_connection(uint32_t connection, const char *root,
				    struct mlg_fw_service **out)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;
	kern_return_t kr;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	kr = IOConnectMapMemory64((io_connect_t)connection, MLG_FW_MAILBOX_MEMORY_TYPE,
				  mach_task_self(), &address, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS || !address)
		return kr == kIOReturnUnsupported || kr == kIOReturnBadArgument
			? -EOPNOTSUPP : -EIO;
	ret = mlg_fw_service_start((void *)(uintptr_t)address, (size_t)size, root, out);
	if (ret) {
		IOConnectUnmapMemory64((io_connect_t)connection, MLG_FW_MAILBOX_MEMORY_TYPE,
				       mach_task_self(), address);
		return ret;
	}
	mlg_fw_service_set_unmap(*out, unmap_connection, connection,
				 (uint64_t)address, (uint64_t)size);
	return 0;
}
