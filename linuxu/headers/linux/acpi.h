/* linuxu: SHIM (third_party/linux/include/linux/acpi.h)
 *
 * Minimal ACPI surface for amdgpu_bios.c (ATRM).
 */
#ifndef __LINUX_ACPI_H
#define __LINUX_ACPI_H

#include <linux/types.h>

typedef u32 acpi_status;
typedef void *acpi_handle;
typedef u8 acpi_uint8;
typedef u16 acpi_uint16;
typedef u32 acpi_uint32;
typedef u64 acpi_uint64;

#define ACPI_OK			0
#define AE_OK			0
#define AE_SUPPORT		0x100

#define ACPI_NAMESEG_SIZE	4
#define ACPI_OEM_ID_SIZE	6
#define ACPI_OEM_TABLE_ID_SIZE 8

/* Master ACPI table header (upstream acpi/actbl1.h shape) */
struct acpi_table_header {
	char signature[ACPI_NAMESEG_SIZE];
	u32 length;
	u8 revision;
	u8 checksum;
	char oem_id[ACPI_OEM_ID_SIZE];
	char oem_table_id[ACPI_OEM_TABLE_ID_SIZE];
	u32 oem_revision;
	char asl_compiler_id[ACPI_NAMESEG_SIZE];
	u32 asl_compiler_revision;
};

#define ACPI_TYPE_INTEGER	4
#define ACPI_TYPE_BUFFER	8

union acpi_object {
	struct {
		u8 type;
		u8 reserved[3];
		u64 integer;
	} integer;
	struct {
		u8 type;
		u8 reserved[3];
		u32 length;
		void *pointer;
	} buffer;
};

struct acpi_object_list {
	u32 count;
	union acpi_object *pointer;
};

struct acpi_buffer {
	u32 length;
	void *pointer;
};

#define ACPI_NAMESPACE_ROOT	NULL

extern acpi_status acpi_get_handle(const char *name,
				    acpi_handle parent, acpi_handle *out);
extern acpi_status acpi_evaluate_integer(acpi_handle obj_handle,
					  const char *method_name,
					  void *args, u64 *return_value);
extern acpi_status acpi_evaluate_object(acpi_handle obj_handle,
					 const char *method_name,
					 struct acpi_object_list *args,
					 struct acpi_buffer *buffer);
extern int acpi_dev_aldrop(acpi_handle handle);

/* ---- ACPI table access (upstream acpi/acpixf.h shape; used by kfd_crat.c) ---- */
extern acpi_status acpi_get_table(const char *signature, u32 instance,
				   struct acpi_table_header **table);
extern void acpi_put_table(struct acpi_table_header *table);
extern acpi_status acpi_get_table_header(const char *signature, u32 instance,
					   struct acpi_table_header **table);

#endif /* __LINUX_ACPI_H */
