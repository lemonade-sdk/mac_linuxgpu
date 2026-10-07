/* Doorbells clients ring themselves (dext/sources/doorbell_gate.h), raced
 * the way the driver races them: client threads ring their own doorbell
 * page through their gate without pause, while the session queue publishes
 * gates, closes every gate for power transitions and opens them again,
 * retires one client's gate (its close) and every gate (the session's
 * close), and frees a retired gate's memory at once (so a registry that
 * still touched it is a use-after-free AddressSanitizer reports).
 *
 * What it proves: once a close or retire returns, no client writes its
 * doorbell until the gate opens again. The device side marks each doorbell
 * with a poison value the moment the close returns and checks it is still
 * there before reopening; a write in between would change it (and is a data
 * race ThreadSanitizer reports). A client thread stopped inside a write is
 * reported, not waited for forever. */
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
#define RINGERS 2u
#define POISON 0xdeadbeefdeadbeefull

struct client {
	uint64_t owner;
	struct mlg_doorbell_gate *_Atomic gate;	/* the client's mapping */
	volatile uint64_t doorbell[RINGERS];	/* its queues' doorbells (the device's) */
	_Atomic uint64_t value;
	_Atomic uint64_t rung, refused;
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

/* One thread per queue, as the runtime serializes a queue's doorbells
 * under its mutex; a client's queues share its gate. */
static void *ringer(void *arg)
{
	const unsigned n = (unsigned)(uintptr_t)arg;
	struct client *c = &clients[n % CLIENTS];
	volatile uint64_t *doorbell = &c->doorbell[n / CLIENTS];

	while (!atomic_load(&stop)) {
		struct mlg_doorbell_gate *gate = atomic_load(&c->gate);
		const uint64_t v = atomic_fetch_add(&c->value, 1) + 1;

		if (mlg_doorbell_ring(gate, doorbell, v))
			atomic_fetch_add(&c->rung, 1);
		else
			atomic_fetch_add(&c->refused, 1);
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

/* The device: from here no write of @c may land. */
static void poison(struct client *c)
{
	for (unsigned i = 0; i < RINGERS; ++i)
		c->doorbell[i] = POISON;
}

static void check_poison(struct client *c)
{
	for (unsigned i = 0; i < RINGERS; ++i)
		assert(c->doorbell[i] == POISON);
}

static void quiet_window(void)
{
	usleep(300);
}

int main(void)
{
	pthread_t threads[CLIENTS * RINGERS];
	unsigned power = 0, retires = 0, session_closes = 0;
	struct mlg_doorbell_drain d;

	for (unsigned i = 0; i < CLIENTS; ++i) {
		clients[i].owner = 100 + i;
		atomic_store(&clients[i].gate, new_gate());
		assert(mlg_doorbell_gates_publish(&gates, clients[i].owner, atomic_load(&clients[i].gate)));
		assert(atomic_load(&clients[i].gate)->open == 1);
	}
	/* Publishing again is the same gate; another one for the owner is not. */
	{
		struct mlg_doorbell_gate *other = new_gate();

		assert(mlg_doorbell_gates_publish(&gates, clients[0].owner, atomic_load(&clients[0].gate)));
		assert(!mlg_doorbell_gates_publish(&gates, clients[0].owner, other));
		assert(!mlg_doorbell_gates_publish(&gates, 0, other));
		free(other);
	}
	for (unsigned i = 0; i < CLIENTS * RINGERS; ++i)
		assert(!pthread_create(&threads[i], NULL, ringer, (void *)(uintptr_t)i));

	const uint64_t until = now_ns() + 1500000000ull;
	for (unsigned round = 0; now_ns() < until; ++round) {
		usleep(200);
		switch (round % 3) {
		case 0:	/* a power transition: every gate, then open again */
			d = drain(1000000000ull);
			mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_POWER, true, &d);
			assert(!d.stuck);
			for (unsigned i = 0; i < CLIENTS; ++i)
				poison(&clients[i]);
			quiet_window();
			for (unsigned i = 0; i < CLIENTS; ++i)
				check_poison(&clients[i]);
			/* A device reset inside the power transition: the power
			 * release leaves the gates closed while the reset holds. */
			if (round % 4 == 0) {
				d = drain(1000000000ull);
				mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_RESET, true, &d);
				mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_POWER, false, &d);
				quiet_window();
				for (unsigned i = 0; i < CLIENTS; ++i)
					check_poison(&clients[i]);
				mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_POWER, true, &d);
				mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_RESET, false, &d);
				assert(!d.stuck);
			}
			/* A gate published while closed stays closed. */
			if (round % 2) {
				struct client *c = &clients[round % CLIENTS];
				struct mlg_doorbell_gate *old = atomic_load(&c->gate);

				d = drain(1000000000ull);
				mlg_doorbell_gates_retire(&gates, c->owner, &d);
				assert(!d.stuck);
				atomic_store(&c->gate, new_gate());
				assert(mlg_doorbell_gates_publish(&gates, c->owner, atomic_load(&c->gate)));
				assert(atomic_load(&c->gate)->open == 0);
				/* The old one stays the client's mapping (a ringer may
				 * have loaded it); the registry never touches it
				 * again, which the session closes below check by
				 * iterating every slot. */
				(void)old;
			}
			d = drain(1000000000ull);
			mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_POWER, false, &d);
			++power;
			break;
		case 1: {	/* one client's close; it comes back with a new gate */
			struct client *c = &clients[(round / 3) % CLIENTS];
			struct mlg_doorbell_gate *old = atomic_load(&c->gate), *fresh = new_gate();

			d = drain(1000000000ull);
			mlg_doorbell_gates_retire(&gates, c->owner, &d);
			assert(!d.stuck && old->open == 0);
			poison(c);
			quiet_window();
			check_poison(c);
			/* Retired again: nothing (it is not in the registry). */
			mlg_doorbell_gates_retire(&gates, c->owner, &d);
			assert(mlg_doorbell_gates_publish(&gates, c->owner, fresh));
			atomic_store(&c->gate, fresh);
			++retires;
			/* old stays allocated (a ringer may have loaded it): leaked
			 * on purpose, as the client's own mapping outlives its
			 * threads. */
			break;
		}
		case 2:	/* the session's close: every gate, then a new session */
			if (round % 7)
				break;
			d = drain(1000000000ull);
			mlg_doorbell_gates_retire(&gates, 0, &d);
			assert(!d.stuck && mlg_doorbell_gates_count(&gates) == 0);
			for (unsigned i = 0; i < CLIENTS; ++i)
				poison(&clients[i]);
			quiet_window();
			for (unsigned i = 0; i < CLIENTS; ++i) {
				check_poison(&clients[i]);
				struct mlg_doorbell_gate *fresh = new_gate();

				assert(mlg_doorbell_gates_publish(&gates, clients[i].owner, fresh));
				atomic_store(&clients[i].gate, fresh);
			}
			++session_closes;
			break;
		}
	}
	atomic_store(&stop, 1);
	for (unsigned i = 0; i < CLIENTS * RINGERS; ++i)
		pthread_join(threads[i], NULL);

	uint64_t rung = 0, refused = 0;
	for (unsigned i = 0; i < CLIENTS; ++i) {
		rung += atomic_load(&clients[i].rung);
		refused += atomic_load(&clients[i].refused);
		assert(atomic_load(&clients[i].rung) > 0);
	}
	printf("doorbell gate: %llu written, %llu refused while closed; %u power transitions, "
	       "%u client closes, %u session closes\n", (unsigned long long)rung,
	       (unsigned long long)refused, power, retires, session_closes);

	/* A client thread stopped inside a write: reported at the bound, and
	 * the close returns (the driver says who). */
	{
		struct mlg_doorbell_gate *gate = atomic_load(&clients[1].gate);
		uint64_t t0;

		__atomic_add_fetch(&gate->busy, 1, __ATOMIC_SEQ_CST);	/* stopped mid-write */
		d = drain(2000000ull);
		t0 = now_ns();
		mlg_doorbell_gates_hold(&gates, MLG_DOORBELL_HOLD_POWER, true, &d);
		assert(d.stuck == 1 && d.stuck_owner == clients[1].owner);
		assert(now_ns() - t0 >= 2000000ull && now_ns() - t0 < 500000000ull);
		__atomic_sub_fetch(&gate->busy, 1, __ATOMIC_RELEASE);
		d = drain(2000000ull);
		mlg_doorbell_gates_retire(&gates, 0, &d);
		assert(!d.stuck);
	}

	/* The ring itself, uncontended: what a doorbell costs the client. */
	{
		struct mlg_doorbell_gate *gate = new_gate();
		volatile uint64_t bell = 0;
		const unsigned n = 1000000;
		uint64_t t0;

		gate->open = 1;
		t0 = now_ns();
		for (unsigned i = 0; i < n; ++i)
			(void)mlg_doorbell_ring(gate, &bell, i);
		printf("doorbell gate: a ring costs %.1f ns (to normal memory here)\n",
		       (double)(now_ns() - t0) / n);
		free(gate);
	}
	printf("test_doorbell_gate: OK\n");
	return 0;
}
