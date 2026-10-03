#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <pthread.h>
#include "dext_heap_backend.h"
#include "fw/fw_table.h"

extern void *linuxu_dbgfs_read(const char *name, struct dentry *parent);

static void test_allocation_failures(void)
{
	for (long fail = 0; fail < 2; fail++) {
		dext_heap_test_fail_after(fail);
		assert(debugfs_create_dir("failed-root", NULL) == NULL);
		dext_heap_test_fail_after(-1);
		assert(debugfs_lookup("failed-root", NULL) == NULL);
		assert(dext_heap_test_live_allocations() == 0);
	}
	struct dentry *root = debugfs_create_dir("root", NULL);
	assert(root);
	size_t baseline = dext_heap_test_live_allocations();
	for (long fail = 0; fail < 2; fail++) {
		dext_heap_test_fail_after(fail);
		assert(debugfs_create_dir("failed-child", root) == NULL);
		dext_heap_test_fail_after(-1);
		assert(debugfs_lookup("failed-child", root) == NULL);
		assert(dext_heap_test_live_allocations() == baseline);
	}
	dext_heap_test_fail_after(0);
	assert(debugfs_change_name(root, "new-name") != 0);
	dext_heap_test_fail_after(-1);
	assert(debugfs_lookup("root", NULL) == root);
	debugfs_remove(root);
	assert(dext_heap_test_live_allocations() == 0);
}

static void test_firmware_ownership(void)
{
	unsigned char blob[32] = {1}, replacement[32] = {2};
	struct fw_entry entry = {"amdgpu/offline.bin", blob, sizeof(blob)};
	assert(fw_table_register(&entry) == 0);
	assert(fw_table_find(entry.name)->blob[0] == 1);
	size_t baseline = dext_heap_test_live_allocations();
	for (long fail = 0; fail < 2; fail++) {
		dext_heap_test_fail_after(fail);
		assert(fw_table_override(entry.name, replacement,
					 sizeof(replacement)) != 0);
		dext_heap_test_fail_after(-1);
		assert(fw_table_find(entry.name)->blob[0] == 1);
		assert(dext_heap_test_live_allocations() == baseline);
	}
	assert(fw_table_override(entry.name, replacement, sizeof(replacement)) == 0);
	assert(fw_table_find(entry.name)->blob[0] == 2);
	assert(dext_heap_test_live_allocations() == baseline);
	fw_table_clear();
	assert(fw_table_count() == 0);
	assert(dext_heap_test_live_allocations() == 0);
}

static void *replace_firmware(void *context)
{
	(void)context;
	unsigned char bytes[32];
	for (int i = 0; i < 300; i++) {
		memset(bytes, i & 1 ? 0x11 : 0x22, sizeof(bytes));
		assert(fw_table_override("amdgpu/lifetime.bin", bytes, sizeof(bytes)) == 0);
	}
	return NULL;
}

static void test_firmware_lifetimes(void)
{
	unsigned char bytes[32];
	memset(bytes, 0x11, sizeof(bytes));
	assert(fw_table_override("amdgpu/lifetime.bin", bytes, sizeof(bytes)) == 0);
	const struct firmware *held;
	assert(request_firmware_direct(&held, "amdgpu/lifetime.bin", NULL) == 0);
	fw_table_clear();
	assert(held->size == sizeof(bytes) && memcmp(held->data, bytes, sizeof(bytes)) == 0);
	release_firmware(held);
	assert(fw_table_override("amdgpu/lifetime.bin", bytes, sizeof(bytes)) == 0);
	struct firmware *builtin = kzalloc(sizeof(*builtin), GFP_KERNEL);
	assert(builtin && firmware_request_builtin(builtin, "amdgpu/lifetime.bin"));
	assert(memcmp(builtin->data, bytes, sizeof(bytes)) == 0);
	release_firmware(builtin);
	for (long fail = 0; fail < 2; fail++) {
		size_t baseline = dext_heap_test_live_allocations();
		dext_heap_test_fail_after(fail);
		held = (void *)1;
		assert(request_firmware_direct(&held, "amdgpu/lifetime.bin", NULL) != 0);
		assert(held == NULL);
		dext_heap_test_fail_after(-1);
		assert(dext_heap_test_live_allocations() == baseline);
	}
	pthread_t writer;
	assert(pthread_create(&writer, NULL, replace_firmware, NULL) == 0);
	for (int i = 0; i < 300; i++) {
		assert(request_firmware_direct(&held, "amdgpu/lifetime.bin", NULL) == 0);
		assert(held->size == sizeof(bytes));
		assert(held->data[0] == 0x11 || held->data[0] == 0x22);
		for (size_t j = 1; j < held->size; j++)
			assert(held->data[j] == held->data[0]);
		release_firmware(held);
	}
	assert(pthread_join(writer, NULL) == 0);
	fw_table_clear();
	assert(dext_heap_test_live_allocations() == 0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--remove-only") == 0) {
		struct dentry *root = debugfs_create_dir("dri", NULL);
		assert(root);
		debugfs_remove(root);
		return 0;
	}
	char temporary_name[] = "registers";
	int value = 42;
	static const struct file_operations fops = {0};
	struct dentry *root = debugfs_create_dir("dri", NULL);
	struct dentry *gpu = debugfs_create_dir("card0", root);
	struct dentry *file = debugfs_create_file_full(temporary_name, 0444,
						      gpu, &value, NULL, &fops);
	assert(root && gpu && file);
	assert(d_inode(root) && d_inode(gpu) && d_inode(file));
	assert(d_inode(file)->i_private == &value);
	assert(d_inode(file)->i_fop == &fops);
	assert(file->d_parent == gpu);
	memset(temporary_name, 'x', sizeof(temporary_name) - 1);
	assert(debugfs_lookup("registers", gpu) == file);
	assert(linuxu_dbgfs_read("registers", gpu) == &value);
	assert(debugfs_change_name(file, "regs%d", 1) == 0);
	assert(debugfs_lookup("registers", gpu) == NULL);
	assert(debugfs_lookup("regs1", gpu) == file);
	debugfs_remove(root);
	assert(debugfs_lookup("dri", NULL) == NULL);
	assert(debugfs_lookup("card0", root) == NULL);
	assert(debugfs_lookup("regs1", gpu) == NULL);
	assert(debugfs_create_file_full("orphan", 0444, gpu, NULL, NULL,
						 NULL) == NULL);
	root = debugfs_create_dir("dri", NULL);
	assert(root && d_inode(root));
	debugfs_remove(root);
	assert(dext_heap_test_live_allocations() == 0);
	test_allocation_failures();
	test_firmware_ownership();
	test_firmware_lifetimes();
	assert(dext_heap_test_live_bytes() == 0);
	puts("DriverKit heap: debugfs recursive teardown, firmware replacement and allocation failures passed");
	return 0;
}
