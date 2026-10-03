/* linuxu: SHIM (third_party/linux/include/linux/seqlock.h)
 *
 * Sequence readers retry across writes; writers are serialized by the
 * owning lock. The counter operations and barriers live in sync.c.
 */
#ifndef _LINUX_SEQLOCK_H
#define _LINUX_SEQLOCK_H

#include <linux/spinlock.h>
#include <linux/atomic.h>

#define SEQCNT_ZERO(name) { .seqcount = 0 }

#define read_seqcount_begin(s)	seqcount_raw_read_begin(s)
#define read_seqcount_retry(s, s1)	seqcount_retry(s, s1)

/* writer: begin = even->odd transition, end = odd->even */
#define write_seqcount_begin(s)	seqcount_lock_begin(s)
#define write_seqcount_end(s)	seqcount_unlock(s)

/* ---- seqlock (seqcount + spinlock) ---- */
struct seqlock {
	struct seqcount s;
	spinlock_t lock;
};

typedef struct seqlock seqlock_t;

#define __SEQCOUNT_UNLOCKED(sl) \
	{ .s = { .seqcount = 0 }, .lock = ___SPIN_LOCK_INITIALIZER(sl.lock) }
#define __SEQLOCK_UNLOCKED(sl)	__SEQCOUNT_UNLOCKED(sl)

#define DEFINE_SEQLOCK(sl) seqlock_t sl = __SEQLOCK_UNLOCKED(sl)

static inline void seqlock_init(seqlock_t *sl)
{
	seqcount_init(&sl->s);
	spin_lock_init(&sl->lock);
}

static inline unsigned read_seqbegin(const seqlock_t *sl)
{
	return read_seqcount_begin(&sl->s);
}

static inline bool read_seqretry(const seqlock_t *sl, unsigned start)
{
	return read_seqcount_retry(&sl->s, start);
}

static inline void write_seqlock(seqlock_t *sl)
{
	spin_lock(&sl->lock);
	write_seqcount_begin(&sl->s);
}

static inline void write_seqlock_done(seqlock_t *sl)
{
	write_seqcount_end(&sl->s);
	spin_unlock(&sl->lock);
}
#define write_sequnlock(sl)		write_seqlock_done(sl)

#endif /* _LINUX_SEQLOCK_H */
