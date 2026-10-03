/* Anonymous shared-memory files own a persistent sparse cache of CPU pages.
 * The cache is resident in this process; no disk swap space is advertised. */
#include <pthread.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/err.h>

struct linuxu_shmem {
	struct linuxu_shmem *next;
	struct address_space *mapping;
	struct xarray pages;
	unsigned long count;
};
static pthread_mutex_t shmem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct linuxu_shmem *shmem_files;

static struct linuxu_shmem *shmem_find(struct address_space *mapping)
{
	for (struct linuxu_shmem *file = shmem_files; file; file = file->next)
		if (file->mapping == mapping) return file;
	return NULL;
}

static int shmem_release(struct inode *inode, struct file *file)
{
	(void)inode;
	struct linuxu_shmem *backing = file->private_data;
	pthread_mutex_lock(&shmem_lock);
	struct linuxu_shmem **link = &shmem_files;
	while (*link && *link != backing) link = &(*link)->next;
	if (*link) *link = backing->next;
	file->f_mapping->i_private = NULL;
	file->f_mapping->nrpages = 0;
	pthread_mutex_unlock(&shmem_lock);
	unsigned long index = 0;
	struct page *page;
	while ((page = xa_find(&backing->pages, &index, ULONG_MAX, 0))) {
		xa_erase(&backing->pages, index);
		page->mapping = NULL;
		put_page(page);
	}
	xa_destroy(&backing->pages);
	kfree(backing);
	return 0;
}
static const struct file_operations shmem_fops = { .release = shmem_release };

struct file *shmem_file_setup(const char *name, loff_t size, vma_flags_t flags)
{
	(void)flags;
	if (!name || size < 0) return ERR_PTR(-EINVAL);
	struct linuxu_shmem *backing = kzalloc(sizeof(*backing), GFP_KERNEL);
	if (!backing) return ERR_PTR(-ENOMEM);
	xa_init(&backing->pages);
	struct file *file = anon_inode_getfile(name, &shmem_fops, backing, O_RDWR);
	if (IS_ERR(file)) { kfree(backing); return file; }
	backing->mapping = file->f_mapping;
	backing->count = (unsigned long)size / PAGE_SIZE + !!((unsigned long)size % PAGE_SIZE);
	file->f_inode->i_size = size;
	atomic_long_set(&file->f_mapping->i_size, size);
	file->f_mapping->gfp_mask = GFP_HIGHUSER;
	file->f_mapping->i_private = backing;
	pthread_mutex_lock(&shmem_lock);
	backing->next = shmem_files;
	shmem_files = backing;
	pthread_mutex_unlock(&shmem_lock);
	return file;
}

struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
		loff_t size, vma_flags_t flags)
{
	(void)mnt;
	return shmem_file_setup(name, size, flags);
}

gfp_t mapping_gfp_mask(struct address_space *mapping)
{
	return mapping ? mapping->gfp_mask : GFP_KERNEL;
}

struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
		pgoff_t index, gfp_t gfp)
{
	struct page *page;
	pthread_mutex_lock(&shmem_lock);
	struct linuxu_shmem *backing = shmem_find(mapping);
	if (!backing || index >= backing->count) {
		pthread_mutex_unlock(&shmem_lock);
		return ERR_PTR(-EINVAL);
	}
	page = xa_load(&backing->pages, index);
	if (!page) {
		page = linuxu_alloc_cpu_page(gfp | __GFP_ZERO);
		if (!page) { pthread_mutex_unlock(&shmem_lock); return ERR_PTR(-ENOMEM); }
		page->index = index;
		page->mapping = mapping;
		__atomic_fetch_or(&page->flags, 1UL << 11, __ATOMIC_RELAXED);
		int error = xa_err(xa_store(&backing->pages, index, page, gfp));
		if (error) {
			page->mapping = NULL;
			put_page(page);
			pthread_mutex_unlock(&shmem_lock);
			return ERR_PTR(error);
		}
		mapping->nrpages++;
	}
	/* The xarray keeps its initial reference; each read returns another. */
	get_page(page);
	pthread_mutex_unlock(&shmem_lock);
	return page;
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t last)
{
	if (!inode || start < 0 || (last < start && last != -1)) return;
	unsigned long first_index = (unsigned long)start / PAGE_SIZE;
	unsigned long last_index = last == -1 ? ULONG_MAX : (unsigned long)last / PAGE_SIZE;
	unsigned long index = first_index;
	for (;;) {
		pthread_mutex_lock(&shmem_lock);
		struct linuxu_shmem *backing = shmem_find(inode->i_mapping);
		struct page *page = backing ? xa_find(&backing->pages, &index, last_index, 0) : NULL;
		if (!page) { pthread_mutex_unlock(&shmem_lock); return; }
		unsigned long begin = index == first_index ? (unsigned long)start % PAGE_SIZE : 0;
		unsigned long end = index == last_index && last != -1 ? (unsigned long)last % PAGE_SIZE + 1 : PAGE_SIZE;
		if (!begin && end == PAGE_SIZE) {
			xa_erase(&backing->pages, index);
			page->mapping = NULL;
			inode->i_mapping->nrpages--;
			pthread_mutex_unlock(&shmem_lock);
			put_page(page);
		} else {
			get_page(page);
			pthread_mutex_unlock(&shmem_lock);
			lock_page(page);
			memset((char *)page_address(page) + begin, 0, end - begin);
			unlock_page(page);
			put_page(page);
		}
		if (index == last_index || index == ULONG_MAX) return;
		index++;
	}
}

int shmem_writeout(struct folio *folio, void *plug, void *data)
{
	(void)plug; (void)data;
	/* TTM treats writeout as best effort and unlocks on error. Preserve
	 * dirty state: resident pages are persistent, but have no disk backing. */
	folio_mark_dirty(folio);
	return -EAGAIN;
}
