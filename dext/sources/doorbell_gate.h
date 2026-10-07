/* Doorbells a client rings itself, as a Linux process rings the doorbell
 * page it mmaps from /dev/kfd (from build MLG_DIRECT_DOORBELL_BUILD).
 *
 * A session client on the KFD path maps two things from the driver:
 *   MLG_DOORBELL_MEMORY_TYPE       its KFD process's doorbell slice, a page
 *                                  of the doorbell BAR holding only that
 *                                  process's doorbells (map it uncached:
 *                                  kIOMapInhibitCache)
 *   MLG_DOORBELL_GATE_MEMORY_TYPE  its gate, a page of host memory the
 *                                  driver and the client share
 * AQLQueueCreate (56) asked for three outputs answers out[2] = the queue's
 * doorbell, a byte offset in the slice, or MLG_DOORBELL_NONE for a queue
 * the client does not ring itself (a legacy queue); those are rung with
 * AQLQueueKick (57) as before.
 *
 * A store to the GPU's BARs once the GPU stops answering (it powers down,
 * the session isolates or resets it, it leaves the Thunderbolt link) can
 * panic the Mac. So every doorbell write goes through the gate:
 * mlg_doorbell_ring counts itself busy, writes only while the gate is open,
 * and counts itself out. The driver closes a client's gate before any of
 * that (a power transition, a device reset, the session's close or
 * quarantine, a removal, the client's close) and then waits until busy is zero: from then on the
 * client writes no doorbell. Dekker's protocol, both sides sequentially
 * consistent: either the client sees the gate closed, or the driver sees
 * it busy and waits. A closed gate writes nothing; the client asks the
 * driver instead (AQLQueueKick), which answers what the device can take
 * (kIOReturnOffline while it suspends, NoDevice once it is gone).
 *
 * The same header holds the driver's registry of open gates (one per
 * client, the session queue's), generic so the offline test runs it under
 * ThreadSanitizer against client threads ringing. */
#ifndef MAC_LINUXGPU_DOORBELL_GATE_H
#define MAC_LINUXGPU_DOORBELL_GATE_H

#include <stdbool.h>
#include <stdint.h>

#define MLG_DIRECT_DOORBELL_BUILD 264u
#define MLG_DOORBELL_MEMORY_TYPE 0x4442u	/* 'DB' */
#define MLG_DOORBELL_GATE_MEMORY_TYPE 0x4447u	/* 'DG' */
#define MLG_DOORBELL_NONE UINT64_MAX
#define MLG_DOORBELL_GATE_MAGIC 0x6d6c6764u	/* 'mlgd' */
#define MLG_DOORBELL_GATE_BYTES 16384u

struct mlg_doorbell_gate {
	uint32_t magic;		/* the driver's, set before the client maps it */
	uint32_t open;		/* the driver's: 1 while the client may ring */
	uint32_t busy;		/* the client's: doorbell writes in progress */
	uint32_t reserved;
	uint64_t closes;	/* the driver's: how often it was closed */
};

/* Ring, packets and write index are coherent host memory the GPU reads
 * after the doorbell: ordered before it as Linux's writeq() orders them
 * (__iowmb, a DMB OSHST on arm64). */
static inline void mlg_doorbell_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshst" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* The client: write @value to @doorbell (64-bit doorbells, SOC15) unless
 * the gate is closed. True when written. */
static inline bool mlg_doorbell_ring(struct mlg_doorbell_gate *gate, volatile uint64_t *doorbell,
				     uint64_t value)
{
	bool rung = false;

	__atomic_add_fetch(&gate->busy, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&gate->open, __ATOMIC_SEQ_CST)) {
		mlg_doorbell_barrier();
		*doorbell = value;
		rung = true;
	}
	__atomic_sub_fetch(&gate->busy, 1, __ATOMIC_RELEASE);
	return rung;
}

/* ---- the driver's registry ---- */

#ifndef MLG_DOORBELL_GATE_SLOTS
#define MLG_DOORBELL_GATE_SLOTS 64u
#endif

/* Why every gate is closed: each reason is held and released on its own,
 * and the gates open only while none is held. */
#define MLG_DOORBELL_HOLD_POWER 1u	/* a power transition: the device takes no work */
#define MLG_DOORBELL_HOLD_RESET 2u	/* a device reset: the BARs stop decoding */

struct mlg_doorbell_gates {
	bool lock;
	uint32_t holds;		/* MLG_DOORBELL_HOLD_* held */
	struct {
		uint64_t owner;	/* 0: free */
		struct mlg_doorbell_gate *gate;
	} slots[MLG_DOORBELL_GATE_SLOTS];
};

/* How a close waits for a client's write in progress: it lasts a few
 * instructions and a posted store, unless the client's thread was
 * preempted inside it. @now gives nanoseconds; the wait spins (it may run
 * on DriverKit's delivery thread, which must not sleep). */
struct mlg_doorbell_drain {
	uint64_t (*now)(void);
	uint64_t bound_ns;
	uint64_t stuck_owner;	/* out: a client still busy at the bound, or 0 */
	uint32_t stuck;		/* out: how many */
};

static inline void mlg_doorbell_gates_lock(struct mlg_doorbell_gates *g)
{
	while (__atomic_test_and_set(&g->lock, __ATOMIC_ACQUIRE)) {
#if defined(__aarch64__)
		__asm__ volatile("yield");
#endif
	}
}

static inline void mlg_doorbell_gates_unlock(struct mlg_doorbell_gates *g)
{
	__atomic_clear(&g->lock, __ATOMIC_RELEASE);
}

static inline void mlg_doorbell_gate_close(struct mlg_doorbell_gate *gate)
{
	if (__atomic_exchange_n(&gate->open, 0u, __ATOMIC_SEQ_CST))
		__atomic_add_fetch(&gate->closes, 1, __ATOMIC_RELAXED);
}

/* Caller holds the lock; the gate is closed. Waits for its writes in
 * progress, at most until @deadline. */
static inline void mlg_doorbell_gate_drain(struct mlg_doorbell_gate *gate, uint64_t owner,
					   struct mlg_doorbell_drain *d, uint64_t deadline)
{
	while (__atomic_load_n(&gate->busy, __ATOMIC_SEQ_CST)) {
		if (d->now() >= deadline) {
			if (!d->stuck_owner)
				d->stuck_owner = owner;
			d->stuck++;
			return;
		}
#if defined(__aarch64__)
		__asm__ volatile("yield");
#endif
	}
}

/* The session queue: @owner's gate, open unless a reason is held.
 * False when every slot is held (the client then rings through the
 * driver). */
static inline bool mlg_doorbell_gates_publish(struct mlg_doorbell_gates *g, uint64_t owner,
					      struct mlg_doorbell_gate *gate)
{
	bool published = false;

	if (!owner || !gate)
		return false;
	mlg_doorbell_gates_lock(g);
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i)
		if (g->slots[i].owner == owner) {
			published = g->slots[i].gate == gate;
			goto out;
		}
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i) {
		if (g->slots[i].owner)
			continue;
		g->slots[i].owner = owner;
		g->slots[i].gate = gate;
		__atomic_store_n(&gate->open, g->holds ? 0u : 1u, __ATOMIC_SEQ_CST);
		published = true;
		break;
	}
out:
	mlg_doorbell_gates_unlock(g);
	return published;
}

/* Close and forget @owner's gate (owner 0: every gate, the session's close
 * or the device's removal), waiting for writes in progress. From the
 * return on, those clients write no doorbell, and the gates' memory may
 * go. */
static inline void mlg_doorbell_gates_retire(struct mlg_doorbell_gates *g, uint64_t owner,
					     struct mlg_doorbell_drain *d)
{
	const uint64_t deadline = d->now() + d->bound_ns;

	mlg_doorbell_gates_lock(g);
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i) {
		if (!g->slots[i].owner || (owner && g->slots[i].owner != owner))
			continue;
		mlg_doorbell_gate_close(g->slots[i].gate);
	}
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i) {
		if (!g->slots[i].owner || (owner && g->slots[i].owner != owner))
			continue;
		mlg_doorbell_gate_drain(g->slots[i].gate, g->slots[i].owner, d, deadline);
		g->slots[i].owner = 0;
		g->slots[i].gate = 0;
	}
	mlg_doorbell_gates_unlock(g);
}

/* Hold @reason (every gate closes; a power transition, a device reset) or
 * release it (the gates open once no reason is held). Holding waits for
 * writes in progress: from the return on, no client writes a doorbell
 * until every reason is released. */
static inline void mlg_doorbell_gates_hold(struct mlg_doorbell_gates *g, uint32_t reason, bool hold,
					   struct mlg_doorbell_drain *d)
{
	const uint64_t deadline = d->now() + d->bound_ns;
	bool closed;

	mlg_doorbell_gates_lock(g);
	if (hold)
		g->holds |= reason;
	else
		g->holds &= ~reason;
	closed = g->holds != 0;
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i) {
		if (!g->slots[i].owner)
			continue;
		if (closed)
			mlg_doorbell_gate_close(g->slots[i].gate);
		else
			__atomic_store_n(&g->slots[i].gate->open, 1u, __ATOMIC_SEQ_CST);
	}
	if (closed)
		for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i)
			if (g->slots[i].owner)
				mlg_doorbell_gate_drain(g->slots[i].gate, g->slots[i].owner, d, deadline);
	mlg_doorbell_gates_unlock(g);
}

static inline uint32_t mlg_doorbell_gates_count(struct mlg_doorbell_gates *g)
{
	uint32_t n = 0;

	mlg_doorbell_gates_lock(g);
	for (uint32_t i = 0; i < MLG_DOORBELL_GATE_SLOTS; ++i)
		n += g->slots[i].owner != 0;
	mlg_doorbell_gates_unlock(g);
	return n;
}

#endif
