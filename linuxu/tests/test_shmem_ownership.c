/* Offline regression using the unchanged upstream TTM backup implementation. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/shmem_fs.h>
#include <linux/file.h>
#include <linux/page-flags.h>
#include <linux/rcupdate.h>
#include <drm/ttm/ttm_backup.h>

static struct file *concurrent_file;
static struct page *concurrent_pages[8];
static void *read_same_index(void *argument)
{
	unsigned long slot = (unsigned long)argument;
	concurrent_pages[slot] = shmem_read_mapping_page_gfp(concurrent_file->f_mapping, 2, GFP_KERNEL);
	return NULL;
}

int main(void)
{
	assert(linuxu_page_pool_extend(32) == 0);
	assert(PTR_ERR(shmem_file_setup("bad", -1, EMPTY_VMA_FLAGS)) == -EINVAL);
	struct file *file = ttm_backup_shmem_create(4 * PAGE_SIZE);
	struct file *other = ttm_backup_shmem_create(4 * PAGE_SIZE);
	assert(!IS_ERR(file) && !IS_ERR(other));
	struct page *source = alloc_page(GFP_KERNEL), *dest = alloc_page(GFP_KERNEL);
	assert(source && dest);
	unsigned char *bytes = page_address(source);
	for (unsigned i = 0; i < PAGE_SIZE; i++) bytes[i] = (i * 17 + i / 128) & 255;
	assert(ttm_backup_backup_page(file, source, true, 1, GFP_KERNEL, GFP_KERNEL) == 2);
	memset(page_address(dest), 0xa5, PAGE_SIZE);
	assert(ttm_backup_copy_page(file, dest, 2, false, GFP_KERNEL) == 0);
	assert(!memcmp(page_address(source), page_address(dest), PAGE_SIZE));
	struct page *saved = shmem_read_mapping_page_gfp(file->f_mapping, 1, GFP_KERNEL);
	assert(!IS_ERR(saved) && saved->mapping == file->f_mapping && saved->index == 1);
	assert(PageDirty(saved) && PageSwapBacked(saved) && !PageLocked(saved));
	assert(page_ref_count(saved) == 2 && file->f_mapping->nrpages == 1);
	struct page *isolated = shmem_read_mapping_page_gfp(other->f_mapping, 1, GFP_KERNEL);
	assert(!IS_ERR(isolated) && isolated != saved);
	for (unsigned i = 0; i < PAGE_SIZE; i++) assert(!((unsigned char *)page_address(isolated))[i]);
	put_page(isolated);

	shmem_truncate_range(file_inode(file), PAGE_SIZE + 19, PAGE_SIZE + 72);
	for (unsigned i = 0; i < PAGE_SIZE; i++)
		assert(((unsigned char *)page_address(saved))[i] == (i >= 19 && i <= 72 ? 0 : bytes[i]));
	put_page(saved);
	ttm_backup_drop(file, 2);
	assert(file->f_mapping->nrpages == 0);
	saved = shmem_read_mapping_page_gfp(file->f_mapping, 1, GFP_KERNEL);
	assert(!IS_ERR(saved));
	for (unsigned i = 0; i < PAGE_SIZE; i++) assert(!((unsigned char *)page_address(saved))[i]);
	put_page(saved);
	assert(PTR_ERR(shmem_read_mapping_page_gfp(file->f_mapping, 4, GFP_KERNEL)) == -EINVAL);
	assert(PTR_ERR(shmem_read_mapping_page_gfp(NULL, 0, GFP_KERNEL)) == -EINVAL);

	concurrent_file = file;
	pthread_t threads[8];
	for (unsigned long i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, read_same_index, (void *)i));
	for (unsigned i = 0; i < 8; i++) pthread_join(threads[i], NULL);
	for (unsigned i = 0; i < 8; i++) {
		assert(!IS_ERR(concurrent_pages[i]) && concurrent_pages[i] == concurrent_pages[0]);
		put_page(concurrent_pages[i]);
	}
	assert(file->f_mapping->nrpages == 2);
	saved = shmem_read_mapping_page_gfp(file->f_mapping, 1, GFP_KERNEL);
	memset(page_address(saved), 0x5b, PAGE_SIZE);
	ttm_backup_fini(file);
	assert(!saved->mapping && page_ref_count(saved) == 1);
	for (unsigned i = 0; i < PAGE_SIZE; i++) assert(((unsigned char *)page_address(saved))[i] == 0x5b);
	put_page(saved);
	ttm_backup_fini(other);
	put_page(source);
	put_page(dest);

	/* A full descriptor pool is a normal ERR_PTR allocation failure, and
	 * a later retry must work without a poisoned cache entry. */
	struct page *held[32];
	for (unsigned i = 0; i < 32; i++) { held[i] = alloc_page(0); assert(held[i]); }
	file = ttm_backup_shmem_create(PAGE_SIZE);
	assert(!IS_ERR(file));
	assert(PTR_ERR(shmem_read_mapping_page_gfp(file->f_mapping, 0, 0)) == -ENOMEM);
	assert(file->f_mapping->nrpages == 0);
	put_page(held[0]);
	saved = shmem_read_mapping_page_gfp(file->f_mapping, 0, 0);
	assert(!IS_ERR(saved));
	put_page(saved);
	fput(file);
	for (unsigned i = 1; i < 32; i++) put_page(held[i]);
	rcu_barrier();
	puts("shmem: unchanged TTM backup/restore, truncation, concurrent cache ownership, close references and exhaustion passed");
}
