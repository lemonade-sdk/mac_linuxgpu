/* The queues whose doorbell a client may ring on the delivery thread
 * (AQLQueueKick's synchronous form, session_state.h).
 *
 * DriverKit delivers every client's calls on one thread, which must never
 * sleep (build 241). A doorbell rung there cannot take the session queue's
 * locks or look at its tables, which that queue changes without locks. So
 * the queues that may be rung there are published here, under a spinlock
 * that nothing holds across anything that sleeps: the session queue
 * publishes a queue once it exists and retires it before anything of it
 * goes (its DESTROY_QUEUE, its owner's close, the device's stop). A ring
 * holds the lock across its doorbell write, so once retire returns no
 * doorbell of that queue is written and its memory may go. The delivery
 * thread waits at most for one publish or retire (constant time); the
 * session queue at most for one doorbell write.
 *
 * Header only, generic (the queue is opaque, the ring a callback) so the
 * offline test runs it under ThreadSanitizer and AddressSanitizer. */
#ifndef MAC_LINUXGPU_KICK_TABLE_H
#define MAC_LINUXGPU_KICK_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef KICK_TABLE_SLOTS
#define KICK_TABLE_SLOTS 1024u
#endif

struct kick_slot {
	uint64_t handle;	/* 0: free */
	uint64_t owner;		/* the client that created the queue */
	void *queue;
};

struct kick_table {
	bool lock;
	uint32_t holds;		/* no ring at all while any is held (KICK_HOLD_*) */
	uint32_t used;		/* slots [0, used) may be taken */
	struct kick_slot slots[KICK_TABLE_SLOTS];
};

static inline void kick_table_lock(struct kick_table *t)
{
	while (__atomic_test_and_set(&t->lock, __ATOMIC_ACQUIRE)) {
#if defined(__aarch64__)
		__asm__ volatile("yield");
#endif
	}
}

static inline void kick_table_unlock(struct kick_table *t)
{
	__atomic_clear(&t->lock, __ATOMIC_RELEASE);
}

/* The session queue: @queue of @owner may be rung by @handle (nonzero,
 * unique). False when the table is full (the queue is then rung only
 * through the session queue). */
static inline bool kick_table_publish(struct kick_table *t, uint64_t handle, uint64_t owner, void *queue)
{
	bool published = false;

	if (!handle || !queue)
		return false;
	kick_table_lock(t);
	for (uint32_t i = 0; i < KICK_TABLE_SLOTS; ++i) {
		struct kick_slot *slot = &t->slots[i];

		if (slot->handle)
			continue;
		slot->handle = handle;
		slot->owner = owner;
		slot->queue = queue;
		if (i + 1 > t->used)
			t->used = i + 1;
		published = true;
		break;
	}
	kick_table_unlock(t);
	return published;
}

static inline void kick_table_clear_slot(struct kick_table *t, uint32_t i)
{
	t->slots[i].handle = 0;
	t->slots[i].owner = 0;
	t->slots[i].queue = 0;
	while (t->used && !t->slots[t->used - 1].handle)
		t->used--;
}

/* The session queue, before the queue named @handle goes: from the return
 * on, no doorbell of it is written here. */
static inline void kick_table_retire(struct kick_table *t, uint64_t handle)
{
	if (!handle)
		return;
	kick_table_lock(t);
	for (uint32_t i = 0; i < t->used; ++i)
		if (t->slots[i].handle == handle)
			kick_table_clear_slot(t, i);
	kick_table_unlock(t);
}

/* ... every queue of @owner (its close), or of every owner (owner 0: the
 * device's stop). */
static inline void kick_table_retire_owner(struct kick_table *t, uint64_t owner)
{
	kick_table_lock(t);
	for (uint32_t i = 0; i < t->used; ++i)
		if (t->slots[i].handle && (!owner || t->slots[i].owner == owner))
			kick_table_clear_slot(t, i);
	kick_table_unlock(t);
}

/* Why nothing may be rung: each reason is held and released on its own,
 * and the table rings only while none is held. */
#define KICK_HOLD_POWER 1u	/* a power transition: the device takes no work */
#define KICK_HOLD_RESET 2u	/* a device reset: the BARs stop decoding */

/* Hold (or release) @reason. While any reason is held nothing is rung
 * here; holding also waits for a ring in progress: from the return on, the
 * device sees no doorbell from this path. */
static inline void kick_table_hold(struct kick_table *t, uint32_t reason, bool hold)
{
	kick_table_lock(t);
	if (hold)
		t->holds |= reason;
	else
		t->holds &= ~reason;
	kick_table_unlock(t);
}

/* The delivery thread: rings @handle of @owner with @packet through @ring,
 * under the lock. -ENOENT-like @absent when no such queue of that owner is
 * published (the session queue must answer), else what @ring returned. */
static inline int kick_table_ring(struct kick_table *t, uint64_t owner, uint64_t handle, uint64_t packet,
				  int (*ring)(void *queue, uint64_t packet), int absent)
{
	int r = absent;

	if (!handle)
		return absent;
	kick_table_lock(t);
	for (uint32_t i = 0; !t->holds && i < t->used; ++i) {
		const struct kick_slot *slot = &t->slots[i];

		if (slot->handle != handle)
			continue;
		if (slot->owner == owner)
			r = ring(slot->queue, packet);
		break;
	}
	kick_table_unlock(t);
	return r;
}

#endif
