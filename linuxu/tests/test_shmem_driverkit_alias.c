/* Real shmem/page/vmap ownership plus the production DriverKit bridge, using
 * shared temporary-file pages instead of device or IOKit access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/rcupdate.h>
#include <rt/dart.h>

extern void shmem_test_bridge_start(void);
extern unsigned int shmem_test_cpu_allocations(void);
extern void shmem_test_bridge_finish(void);

int main(void)
{
	assert(!linuxu_page_pool_extend(16));
	shmem_test_bridge_start();
	struct file *file = shmem_file_setup("alias backing", 3 * PAGE_SIZE, EMPTY_VMA_FLAGS);
	assert(!IS_ERR(file));
	struct page *pages[3];
	for (unsigned i = 0; i < 3; i++) {
		pages[i] = shmem_read_mapping_page_gfp(file->f_mapping, i, GFP_KERNEL);
		assert(!IS_ERR(pages[i]));
		memset(page_address(pages[i]), 0x20 + i, PAGE_SIZE);
	}
	assert(shmem_test_cpu_allocations() == 3 && !linuxu_dart_used());
	struct page *scattered[] = {pages[2], pages[0], pages[2], pages[1]};
	unsigned char *alias = vmap(scattered, 4, 0, 0);
	assert(alias && is_vmalloc_addr(alias));
	for (unsigned i = 0; i < 4; i++) {
		assert(vmalloc_to_page(alias + i * PAGE_SIZE + 37) == scattered[i]);
		assert(!memcmp(alias + i * PAGE_SIZE, page_address(scattered[i]), PAGE_SIZE));
	}
	alias[19] = 0x7c;
	assert(alias[2 * PAGE_SIZE + 19] == 0x7c && ((unsigned char *)page_address(pages[2]))[19] == 0x7c);
	((unsigned char *)page_address(pages[0]))[31] = 0xd8;
	assert(alias[PAGE_SIZE + 31] == 0xd8);
	assert(page_ref_count(pages[2]) == 4); /* cache, caller and two aliases */
	assert(!linuxu_dart_used());

	/* Truncating the cache retires its reference, while an existing mapping
	 * keeps the original pages coherent until vunmap. A new cache page is zero. */
	shmem_truncate_range(file_inode(file), 2 * PAGE_SIZE, 3 * PAGE_SIZE - 1);
	struct page *replacement = shmem_read_mapping_page_gfp(file->f_mapping, 2, GFP_KERNEL);
	assert(!IS_ERR(replacement) && replacement != pages[2]);
	assert(!((unsigned char *)page_address(replacement))[19] && alias[19] == 0x7c);
	put_page(replacement);
	fput(file);
	for (unsigned i = 0; i < 3; i++) put_page(pages[i]);
	assert(shmem_test_cpu_allocations() == 3);
	assert(alias[19] == 0x7c && alias[PAGE_SIZE + 31] == 0xd8);
	alias[3 * PAGE_SIZE + 97] = 0xa6;
	assert(((unsigned char *)page_address(pages[1]))[97] == 0xa6);
	vunmap(alias);
	assert(!is_vmalloc_addr(alias) && !vmalloc_to_page(alias));
	assert(!linuxu_dart_used() && !shmem_test_cpu_allocations());
	rcu_barrier();
	shmem_test_bridge_finish();
	puts("DriverKit shmem aliases: shared writes, duplicate pages, truncate/close ownership, zero DART charge and teardown passed");
}
