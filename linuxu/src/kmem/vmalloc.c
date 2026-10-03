/* CPU aliases retain every backing page until the mapping is released. */
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#include <linux/mm.h>
#include <linux/vmalloc.h>
#ifdef LINUXU_DEXT_DK
#include <rt/dext_dma.h>
#endif

struct linuxu_vmap_entry {
	struct linuxu_vmap_entry *next;
	void *address;
	unsigned int count;
	struct page **pages;
	bool platform_alias;
};
static pthread_mutex_t vmap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct linuxu_vmap_entry *vmaps;

void *vmalloc(unsigned long size)
{
	return size ? malloc(size) : NULL;
}

void vfree(const void *addr)
{
	free((void *)addr);
}

void *vmap(struct page **pages, unsigned int count,
	   unsigned long flags, pgprot_t prot)
{
	(void)flags; (void)prot;
	if (!pages || !count || count > SIZE_MAX / PAGE_SIZE ||
		count > SIZE_MAX / sizeof(*pages)) return NULL;
	struct linuxu_vmap_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) return NULL;
	entry->pages = calloc(count, sizeof(*entry->pages));
	if (!entry->pages) { free(entry); return NULL; }
	unsigned int held = 0;
	for (; held < count; held++) {
		if (!get_page(pages[held])) break;
		entry->pages[held] = pages[held];
	}
	if (held != count) goto failed;
	uintptr_t base = (uintptr_t)page_address(pages[0]);
	if (!base || base > UINTPTR_MAX - (size_t)count * PAGE_SIZE) goto failed;
	unsigned int i;
	for (i = 1; i < count; i++)
		if ((uintptr_t)page_address(pages[i]) != base + (size_t)i * PAGE_SIZE) break;
	if (i == count) {
		entry->address = (void *)base;
	} else {
#ifdef LINUXU_DEXT_DK
		const void **addresses = malloc((size_t)count * sizeof(*addresses));
		if (!addresses) goto failed;
		for (i = 0; i < count; i++) addresses[i] = page_address(pages[i]);
		entry->address = dext_dma_vmap_pages(addresses, count);
		free(addresses);
		if (!entry->address) goto failed;
		entry->platform_alias = true;
#else
		goto failed;
#endif
	}
	entry->count = count;
	pthread_mutex_lock(&vmap_lock);
	entry->next = vmaps;
	vmaps = entry;
	pthread_mutex_unlock(&vmap_lock);
	return entry->address;
failed:
	while (held) put_page(entry->pages[--held]);
	free(entry->pages);
	free(entry);
	return NULL;
}

void vunmap(const void *addr)
{
	pthread_mutex_lock(&vmap_lock);
	struct linuxu_vmap_entry **link = &vmaps;
	while (*link && (*link)->address != addr) link = &(*link)->next;
	struct linuxu_vmap_entry *entry = *link;
	if (entry) *link = entry->next;
	pthread_mutex_unlock(&vmap_lock);
	if (!entry) return;
#ifdef LINUXU_DEXT_DK
	if (entry->platform_alias) dext_dma_vunmap_pages(addr);
#endif
	for (unsigned int i = 0; i < entry->count; i++) put_page(entry->pages[i]);
	free(entry->pages);
	free(entry);
}

struct page *vmalloc_to_page(const void *address)
{
	uintptr_t value = (uintptr_t)address;
	struct page *page = NULL;
	pthread_mutex_lock(&vmap_lock);
	for (struct linuxu_vmap_entry *entry = vmaps; entry; entry = entry->next) {
		uintptr_t base = (uintptr_t)entry->address;
		if (value >= base && value - base < (size_t)entry->count * PAGE_SIZE) {
			page = entry->pages[(value - base) / PAGE_SIZE];
			break;
		}
	}
	pthread_mutex_unlock(&vmap_lock);
	return page;
}

bool is_vmalloc_addr(const void *address)
{
	return vmalloc_to_page(address) != NULL;
}
