/* linuxu shim: kmemcheck — heap canary verification for kmalloc/kfree, in
 * debug builds (DEBUG); a release build has none of it (kmemalloc.c).
 * Every kmalloc object is allocated with a leading/trailing canary word and
 * a per-size checksum so the P0 kmemcheck goal (corrupt a byte -> detect)
 * is real.  kmemcheck_verify scans all live allocations. */
#if DEBUG
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/gfp.h>

/* kmemcheck_scan/kmemcheck_enabled/kmemcheck_verify_all: the allocator
 * (kmemalloc.c) is always kmemcheck-instrumented, so these are defined
 * there; this file only carries the canary tracking table. */

/* ---- canary layout ----
 *
 *   [alignment pad][hdr][canary_lo][user payload ...][canary_hi]
 *
 * hdr:        struct kmemcheck_hdr (8-byte aligned)
 * canary_lo:  8 bytes, pattern f^0x11, f = checksum of payload size
 * canary_hi:  8 bytes, pattern f^0x22
 *
 * The payload is aligned to the shim's ARCH_DMA_MINALIGN. The trailing
 * canary immediately follows the payload, so a 1-byte
 * out-of-bounds write past the payload corrupts canary_hi -> detected.
 */
struct kmemcheck_hdr {
	uintptr_t user_ptr;      /* points at start of payload            */
	size_t    size;          /* user-requested size                   */
	size_t    alloc_size;    /* payload + canary_hi bytes             */
	uint64_t  magic;         /* KMEMCHECK_MAGIC if live               */
	uint64_t  canary_lo_pat; /* expected lo pattern                   */
	void     *allocation;   /* original backing allocation           */
	size_t    slot;          /* table index + 1, 0 when not tracked   */
};

#define KMEMCHECK_MAGIC   0xc0ffee00c0ffee00ULL
#define KMEMCHECK_LO_0    0x11ULL
#define KMEMCHECK_HI_0    0x22ULL
#define KMEMCHECK_HDR_ALIGN 8

/* ---- live allocation table ----
 *
 * Tracking is O(1): a header remembers its table slot, and freed slots go
 * on a stack for reuse. Scans walk only the slots ever used. Every
 * kmalloc/kfree of the driver pays this, so it must not grow with the
 * object's size or with the number of live objects. */
struct kmemcheck_rec {
	struct kmemcheck_hdr *hdr;
	int live;
};

#define KMEMCHECK_MAX_RECS 65536
static struct kmemcheck_rec kmemcheck_recs[KMEMCHECK_MAX_RECS];
static uint32_t kmemcheck_free_slots[KMEMCHECK_MAX_RECS];
static uint32_t kmemcheck_nfree;	/* entries on the free stack       */
static uint32_t kmemcheck_used;		/* slots ever handed out           */
static pthread_mutex_t kmemcheck_lock = PTHREAD_MUTEX_INITIALIZER;

/* The canary pattern for a payload size: a 64-bit mix of the size
 * (splitmix64), so sizes differ in every byte of their canaries. */
static uint64_t kmemcheck_pattern(size_t size)
{
	uint64_t h = (uint64_t)size + 0x9e3779b97f4a7c15ULL;

	h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
	h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
	return h ^ (h >> 31);
}

/* kmemcheck_enabled: defined in kmemalloc.c (single definition point). */

/* Called from kmemalloc.c after a successful kmalloc.  Returns 0 on
 * success.  `hdr` must be the leading struct kmemcheck_hdr. */
int kmemcheck_track(struct kmemcheck_hdr *hdr, size_t user_size)
{
	uint64_t pat = kmemcheck_pattern(user_size);
	uint64_t lo = pat ^ KMEMCHECK_LO_0, hi = pat ^ KMEMCHECK_HI_0;
	uint32_t i;

	hdr->size = user_size;
	hdr->magic = KMEMCHECK_MAGIC;
	hdr->canary_lo_pat = lo;
	hdr->slot = 0;

	/* paint canaries */
	memcpy((void *)(hdr->user_ptr - sizeof(uint64_t)), &lo, sizeof(lo));
	/* The trailing canary follows arbitrary-sized payloads and can be unaligned. */
	memcpy((void *)(hdr->user_ptr + user_size), &hi, sizeof(hi));
	/* Publish only initialized metadata to concurrent heap scans. */
	pthread_mutex_lock(&kmemcheck_lock);
	if (kmemcheck_nfree)
		i = kmemcheck_free_slots[--kmemcheck_nfree];
	else if (kmemcheck_used < KMEMCHECK_MAX_RECS)
		i = kmemcheck_used++;
	else
		i = UINT32_MAX;	/* table full: this object goes unchecked */
	if (i != UINT32_MAX) {
		kmemcheck_recs[i].hdr = hdr;
		kmemcheck_recs[i].live = 1;
		hdr->slot = (size_t)i + 1;
	}
	pthread_mutex_unlock(&kmemcheck_lock);
	return 0;
}

/* Called from kmemalloc.c before kfree.  Returns 1 if the canaries are
 * intact (normal free), 0 if corruption was detected. */
int kmemcheck_untrack(struct kmemcheck_hdr *hdr)
{
	int corrupt = 1;

	pthread_mutex_lock(&kmemcheck_lock);
	if (hdr->magic != KMEMCHECK_MAGIC) {
		pthread_mutex_unlock(&kmemcheck_lock);
		return 0;
	}

	uint64_t pat = kmemcheck_pattern(hdr->size);
	uint64_t lo, hi;
	memcpy(&lo, (void *)(hdr->user_ptr - sizeof(uint64_t)), sizeof(lo));
	memcpy(&hi, (void *)(hdr->user_ptr + hdr->size), sizeof(hi));

	if (lo != (pat ^ KMEMCHECK_LO_0) || hi != (pat ^ KMEMCHECK_HI_0))
		corrupt = 0;

	hdr->magic = 0;
	if (hdr->slot && hdr->slot <= kmemcheck_used) {
		const uint32_t i = (uint32_t)(hdr->slot - 1);

		if (kmemcheck_recs[i].live && kmemcheck_recs[i].hdr == hdr) {
			kmemcheck_recs[i].live = 0;
			kmemcheck_recs[i].hdr = NULL;
			kmemcheck_free_slots[kmemcheck_nfree++] = i;
		}
	}
	hdr->slot = 0;
	pthread_mutex_unlock(&kmemcheck_lock);
	return corrupt;
}

/* Scan all live allocations; returns number of corrupted objects.
 * (kmemcheck_verify_all in kmemalloc.c is a thin wrapper over this.) */
int kmemcheck_scan(void)
{
	int corrupt = 0, i;

	pthread_mutex_lock(&kmemcheck_lock);
	for (i = 0; i < (int)kmemcheck_used; i++) {
		struct kmemcheck_hdr *hdr = kmemcheck_recs[i].hdr;

		if (!kmemcheck_recs[i].live || !hdr)
			continue;
		if (hdr->magic != KMEMCHECK_MAGIC) {
			corrupt++;
			continue;
		}
		uint64_t pat = kmemcheck_pattern(hdr->size);
		uint64_t lo, hi;
		memcpy(&lo, (void *)(hdr->user_ptr - sizeof(uint64_t)), sizeof(lo));
		memcpy(&hi, (void *)(hdr->user_ptr + hdr->size), sizeof(hi));
		if (lo != (pat ^ KMEMCHECK_LO_0) ||
		    hi != (pat ^ KMEMCHECK_HI_0))
			corrupt++;
	}
	pthread_mutex_unlock(&kmemcheck_lock);
	return corrupt;
}

/* kmemcheck_verify_all: defined in kmemalloc.c over kmemcheck_scan(). */


/* Total bytes in live tracked payloads (for budget tests). */
size_t kmemcheck_live_bytes(void)
{
	size_t total = 0, i;

	pthread_mutex_lock(&kmemcheck_lock);
	for (i = 0; i < kmemcheck_used; i++)
		if (kmemcheck_recs[i].live && kmemcheck_recs[i].hdr)
			total += kmemcheck_recs[i].hdr->size;
	pthread_mutex_unlock(&kmemcheck_lock);
	return total;
}
#else
#include <stddef.h>
/* Release builds track nothing: no object is live to the checker. */
size_t kmemcheck_live_bytes(void);
size_t kmemcheck_live_bytes(void) { return 0; }
#endif /* DEBUG */
