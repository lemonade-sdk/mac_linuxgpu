/* linuxu: SHIM (third_party/linux/include/linux/folio_batch.h) */
#ifndef __LINUX_FOLIO_BATCH_H
#define __LINUX_FOLIO_BATCH_H
#include <linux/mm.h>

static inline void folio_batch_init(struct folio_batch *l)
{
	l->nr = 0;
}
static inline unsigned int folio_batch_count(const struct folio_batch *l)
{
	return l->nr;
}
static inline bool folio_batch_add(struct folio_batch *l, void *folio)
{
	if (l->nr >= 16)
		return false;
	l->folios[l->nr++] = folio;
	return l->nr < 16;
}
#endif
