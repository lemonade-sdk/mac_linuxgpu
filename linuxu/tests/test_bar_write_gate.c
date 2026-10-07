/* A submission's stores into the BARs as one bracket (mlg_bar_write_begin
 * and mlg_bar_write_end in dext/sources/doorbell_gate.h), raced the way the
 * driver races them: client threads submit without pause, each submission
 * copying its kernel arguments into the client's VRAM kernarg ring with
 * memcpy, storing the HDP flush register and reading it back, and ringing
 * the doorbell (a nested bracket, mlg_doorbell_ring), while the session
 * queue holds every gate for power transitions and resets, retires one
 * client's gate (its close) and every gate (the session's close or a
 * removal).
 *
 * What it proves: once a hold or retire returns, none of a client's
 * stores reaches its ring, its HDP register or its doorbell until the gate
 * opens again, however many stores a submission makes. The device side
 * poisons all three the moment the close returns and checks them before
 * reopening; a store in between changes them (and is a data race
 * ThreadSanitizer reports). */
#define MLG_DOORBELL_GATE_SLOTS 8u
#include "doorbell_gate.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 4u
#define SUBMITTERS 2u		/* queues per client, each its own thread */
#define RING_BYTES 4096u	/* a queue's kernarg ring in VRAM */
#define KERNARG_BYTES 192u	/* one submission's arguments (3 blocks) */
#define POISON 0xa5u
#define POISON64 0xa5a5a5a5a5a5a5a5ull

struct queue {
	uint8_t ring[RING_BYTES];	/* the device's VRAM */
	volatile uint32_t hdp_mem_flush_cntl;	/* the device's register (one per
						 * queue here, so the submitters
						 * do not race each other) */
	volatile uint64_t doorbell;	/* the device's doorbell */
	uint32_t write;			/* the submitter's ring position */
};

struct client {
	uint64_t owner;
	struct mlg_doorbell_gate *_Atomic gate;	/* the client's mapping */
	struct queue queues[SUBMITTERS];
	_Atomic uint64_t value;
	_Atomic uint64_t submitted, refused, nested_refused;
};

static struct mlg_doorbell_gates gates;
static struct client clients[CLIENTS];
static _Atomic int stop;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct mlg_doorbell_drain drain(uint64_t bound_ns)
{
	struct mlg_doorbell_drain d = {now_ns, bound_ns, 0, 0};
	return d;
}

/* One submission as HRX makes it: kernargs, HDP publication, doorbell. */
static void *submitter(void *arg)
{
	const unsigned n = (unsigned)(uintptr_t)arg;
	struct client *c = &clients[n % CLIENTS];
	struct queue *q = &c->queues[n / CLIENTS];
	uint8_t args[KERNARG_BYTES];

	while (!atomic_load(&stop)) {
		struct mlg_doorbell_gate *gate = atomic_load(&c->gate);
		const uint64_t v = atomic_fetch_add(&c->value, 1) + 1;

		memset(args, (int)(v & 0x7f), sizeof(args));
		if (!mlg_bar_write_begin(gate)) {
			atomic_fetch_add(&c->refused, 1);
			continue;
		}
		if (q->write + KERNARG_BYTES > RING_BYTES)
			q->write = 0;
		memcpy(&q->ring[q->write], args, sizeof(args));
		q->write += KERNARG_BYTES;
		q->hdp_mem_flush_cntl = 1u;
		(void)q->hdp_mem_flush_cntl;
		if (!mlg_doorbell_ring(gate, &q->doorbell, v))
			atomic_fetch_add(&c->nested_refused, 1);
		mlg_bar_write_end(gate);
		atomic_fetch_add(&c->submitted, 1);
	}
	return NULL;
}

static struct mlg_doorbell_gate *new_gate(void)
{
	struct mlg_doorbell_gate *gate = calloc(1, sizeof(*gate));

	assert(gate);
	gate->magic = MLG_DOORBELL_GATE_MAGIC;
	return gate;
}

/* The device: from here no store of @c may land. */
static void poison(struct client *c)
{
	for (unsigned i = 0; i < SUBMITTERS; ++i) {
		c->queues[i].hdp_mem_flush_cntl = (uint32_t)POISON64;
		memset(c->queues[i].ring, POISON, RING_BYTES);
		c->queues[i].doorbell = POISON64;
	}
}

static void check_poison(struct client *c)
{
	for (unsigned i = 0; i < SUBMITTERS; ++i) {
		assert(c->queues[i].hdp_mem_flush_cntl == (uint32_t)POISON64);
		for (unsigned b = 0; b < RING_BYTES; ++b)
			assert(c->queues[i].ring[b] == POISON);
		assert(c->queues[i].doorbell == POISON64);
	}
}

static void quiet_window(void)
{
	usleep(300);
}

int main(void)
{
	pthread_t threads[CLIENTS * SUBMITTERS];
	unsigned holds = 0, retires = 0, removals = 0;
	struct mlg_doorbell_drain d;

	for (unsigned i = 0; i < CLIENTS; ++i) {
		clients[i].owner = 100 + i;
		atomic_store(&clients[i].gate, new_gate());
		assert(mlg_doorbell_gates_publish(&gates, clients[i].owner, atomic_load(&clients[i].gate)));
	}
	/* A closed gate opens no bracket and leaves busy as it was. */
	{
		struct mlg_doorbell_gate *g = new_gate();

		assert(!mlg_bar_write_begin(g) && g->busy == 0);
		g->open = 1;
		assert(mlg_bar_write_begin(g) && g->busy == 1);
		assert(mlg_bar_write_begin(g) && g->busy == 2);	/* nested */
		mlg_bar_write_end(g);
		mlg_bar_write_end(g);
		assert(g->busy == 0);
		free(g);
	}
	for (unsigned i = 0; i < CLIENTS * SUBMITTERS; ++i)
		assert(!pthread_create(&threads[i], NULL, submitter, (void *)(uintptr_t)i));

	const uint64_t until = now_ns() + 1500000000ull;
	for (unsigned round = 0; now_ns() < until; ++round) {
		usleep(200);
		switch (round % 3) {
		case 0:	/* a power transition or a reset: every gate */
			d = drain(1000000000ull);
			mlg_doorbell_gates_hold(&gates, round % 2 ? MLG_DOORBELL_HOLD_RESET : MLG_DOORBELL_HOLD_POWER,
						true, &d);
			assert(!d.stuck);
			for (unsigned i = 0; i < CLIENTS; ++i)
				poison(&clients[i]);
			quiet_window();
			for (unsigned i = 0; i < CLIENTS; ++i)
				check_poison(&clients[i]);
			mlg_doorbell_gates_hold(&gates, round % 2 ? MLG_DOORBELL_HOLD_RESET : MLG_DOORBELL_HOLD_POWER,
						false, &d);
			++holds;
			break;
		case 1: {	/* one client's close; it comes back with a new gate */
			struct client *c = &clients[(round / 3) % CLIENTS];

			d = drain(1000000000ull);
			mlg_doorbell_gates_retire(&gates, c->owner, &d);
			assert(!d.stuck);
			poison(c);
			quiet_window();
			check_poison(c);
			atomic_store(&c->gate, new_gate());	/* old one leaked: still mapped */
			assert(mlg_doorbell_gates_publish(&gates, c->owner, atomic_load(&c->gate)));
			++retires;
			break;
		}
		case 2:	/* the session's close or a removal: every gate */
			if (round % 7)
				break;
			d = drain(1000000000ull);
			mlg_doorbell_gates_retire(&gates, 0, &d);
			assert(!d.stuck && mlg_doorbell_gates_count(&gates) == 0);
			for (unsigned i = 0; i < CLIENTS; ++i)
				poison(&clients[i]);
			quiet_window();
			for (unsigned i = 0; i < CLIENTS; ++i)
				check_poison(&clients[i]);
			for (unsigned i = 0; i < CLIENTS; ++i) {
				atomic_store(&clients[i].gate, new_gate());
				assert(mlg_doorbell_gates_publish(&gates, clients[i].owner,
								  atomic_load(&clients[i].gate)));
			}
			++removals;
			break;
		}
	}
	atomic_store(&stop, 1);
	for (unsigned i = 0; i < CLIENTS * SUBMITTERS; ++i)
		pthread_join(threads[i], NULL);

	uint64_t submitted = 0, refused = 0, nested = 0;
	for (unsigned i = 0; i < CLIENTS; ++i) {
		submitted += atomic_load(&clients[i].submitted);
		refused += atomic_load(&clients[i].refused);
		nested += atomic_load(&clients[i].nested_refused);
	}
	assert(holds && retires && removals && submitted && refused);
	printf("test_bar_write_gate: %llu submissions (%llu refused at begin, %llu doorbells refused "
	       "inside an open bracket); %u holds, %u client closes, %u session closes: no store "
	       "after a close\n",
	       (unsigned long long)submitted, (unsigned long long)refused, (unsigned long long)nested, holds,
	       retires, removals);
	return 0;
}
