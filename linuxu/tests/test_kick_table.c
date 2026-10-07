/* Doorbells on the delivery thread (dext/sources/kick_table.h), raced the
 * way the driver races them: delivery threads ring queues of their own
 * client by handle, many of them stale, while the session queue publishes
 * queues, retires them before their DESTROY_QUEUE and frees them the moment
 * retire returns (so a ring after retire is a use-after-free AddressSanitizer
 * reports, and a dead marker ThreadSanitizer runs catch), retires a whole
 * client (its close) and closes the table for power transitions.
 *
 * What it proves: no ring reaches a retired or freed queue, nor a queue of
 * another client, nor any queue while the table is closed; and nothing the
 * session queue does while it sleeps (DESTROY_QUEUE, a close) holds the
 * delivery threads, which keep ringing other queues meanwhile. A delivery
 * thread waits for the session queue only for one publish or retire, and
 * the session queue for one doorbell write: both bounds are measured. */
#define KICK_TABLE_SLOTS 256u
#include "kick_table.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LIVE 0x4c495645u
#define DEAD 0x44454144u
#define OWNERS 4u
#define DELIVERY 4u
#define RECENT 64u

struct fake_queue {
	_Atomic uint32_t magic;
	uint64_t owner;
	_Atomic uint64_t rings;
};

static struct kick_table table;
static _Atomic uint64_t recent[RECENT];	/* handles, published or not */
static _Atomic int stop;
static _Atomic int table_closed;	/* set after a kick_table_hold returned */
static _Atomic uint64_t rings, absent, rings_during_slow_session;
static _Atomic int session_sleeping;
static _Atomic uint64_t max_ring_ns, max_retire_ns;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void note_max(_Atomic uint64_t *max, uint64_t value)
{
	uint64_t seen = atomic_load(max);

	while (value > seen && !atomic_compare_exchange_weak(max, &seen, value))
		;
}

/* The doorbell write: about a microsecond, as a kernel MemoryWrite64 is. */
static int ring(void *queue, uint64_t packet)
{
	struct fake_queue *q = queue;
	const uint64_t until = now_ns() + 1000;

	assert(atomic_load(&q->magic) == LIVE);		/* never a retired queue */
	assert((packet >> 32) == q->owner);		/* never another client's */
	assert(!atomic_load(&table_closed));		/* never while closed */
	while (now_ns() < until)
		;
	atomic_fetch_add(&q->rings, 1);
	return 0;
}

static void *delivery(void *arg)
{
	const uint64_t owner = (uint64_t)(uintptr_t)arg;
	uint64_t seq = 0;
	unsigned seed = (unsigned)owner * 7919u;

	while (!atomic_load(&stop)) {
		const uint64_t handle = atomic_load(&recent[rand_r(&seed) % RECENT]);
		const uint64_t start = now_ns();
		const int r = kick_table_ring(&table, owner, handle, (owner << 32) | (++seq & 0xffffffffu),
					      ring, -ENOENT);

		note_max(&max_ring_ns, now_ns() - start);
		if (r == 0) {
			atomic_fetch_add(&rings, 1);
			if (atomic_load(&session_sleeping))
				atomic_fetch_add(&rings_during_slow_session, 1);
		} else {
			assert(r == -ENOENT);
			atomic_fetch_add(&absent, 1);
		}
	}
	return NULL;
}

struct live { uint64_t handle; struct fake_queue *q; };

static void retire_and_free(struct live *l)
{
	const uint64_t start = now_ns();

	kick_table_retire(&table, l->handle);
	note_max(&max_retire_ns, now_ns() - start);
	atomic_store(&l->q->magic, DEAD);
	free(l->q);
	l->q = NULL;
	l->handle = 0;
}

int main(void)
{
	pthread_t threads[DELIVERY];
	struct live queues[48];
	uint64_t next_handle = 1;
	unsigned seed = 1, created = 0, retired = 0, closes = 0, gates = 0;

	memset(queues, 0, sizeof(queues));
	for (uintptr_t i = 0; i < DELIVERY; ++i)
		assert(!pthread_create(&threads[i], NULL, delivery, (void *)(i % OWNERS + 1)));
	for (unsigned round = 0; round < 3000; ++round) {
		struct live *l = &queues[rand_r(&seed) % 48];

		if (!l->q) {
			/* A queue is created (its KFD CREATE_QUEUE done), then published. */
			struct fake_queue *q = calloc(1, sizeof(*q));

			assert(q);
			atomic_store(&q->magic, LIVE);
			q->owner = rand_r(&seed) % OWNERS + 1;
			l->q = q;
			l->handle = next_handle++;
			assert(kick_table_publish(&table, l->handle, q->owner, q));
			atomic_store(&recent[l->handle % RECENT], l->handle);
			created++;
		} else {
			/* Destroyed: retired, then DESTROY_QUEUE, which may sleep. */
			retire_and_free(l);
			retired++;
			if (round % 97 == 0) {
				atomic_store(&session_sleeping, 1);
				usleep(5000);
				atomic_store(&session_sleeping, 0);
			}
		}
		if (round % 211 == 0) {
			/* A client closes: every queue of it goes. */
			const uint64_t owner = rand_r(&seed) % OWNERS + 1;

			kick_table_retire_owner(&table, owner);
			for (unsigned i = 0; i < 48; ++i)
				if (queues[i].q && queues[i].q->owner == owner) {
					atomic_store(&queues[i].q->magic, DEAD);
					free(queues[i].q);
					queues[i].q = NULL;
					queues[i].handle = 0;
				}
			closes++;
		}
		if (round % 307 == 0) {
			/* A power transition, and a device reset inside it:
			 * nothing rung while either is held, and releasing one
			 * leaves the other holding. */
			kick_table_hold(&table, KICK_HOLD_POWER, true);
			atomic_store(&table_closed, 1);
			kick_table_hold(&table, KICK_HOLD_RESET, true);
			kick_table_hold(&table, KICK_HOLD_POWER, false);
			usleep(100);
			kick_table_hold(&table, KICK_HOLD_POWER, true);
			kick_table_hold(&table, KICK_HOLD_RESET, false);
			usleep(2000);
			atomic_store(&table_closed, 0);
			kick_table_hold(&table, KICK_HOLD_POWER, false);
			gates++;
		}
		if (round % 50 == 0)
			usleep(100);
	}
	/* The device stops: everything retired, nothing rung afterwards. */
	kick_table_retire_owner(&table, 0);
	for (unsigned i = 0; i < 48; ++i)
		if (queues[i].q) {
			atomic_store(&queues[i].q->magic, DEAD);
			free(queues[i].q);
			queues[i].q = NULL;
		}
	assert(table.used == 0);
	usleep(2000);
	atomic_store(&stop, 1);
	for (unsigned i = 0; i < DELIVERY; ++i)
		assert(!pthread_join(threads[i], NULL));
	assert(atomic_load(&rings) > 0 && atomic_load(&absent) > 0);
	/* The delivery threads kept ringing while the session queue slept. */
	assert(atomic_load(&rings_during_slow_session) > 0);
	printf("kick table: %u queues created, %u retired, %u client closes, %u power gates; %llu rings "
	       "(%llu while the session queue slept), %llu absent; longest ring %.1f us, longest retire %.1f us\n",
	       created, retired, closes, gates, (unsigned long long)atomic_load(&rings),
	       (unsigned long long)atomic_load(&rings_during_slow_session),
	       (unsigned long long)atomic_load(&absent), atomic_load(&max_ring_ns) / 1e3,
	       atomic_load(&max_retire_ns) / 1e3);
	return 0;
}
