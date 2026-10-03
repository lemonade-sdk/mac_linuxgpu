/* test_irq.c — IRQ path (REAL): register a handler on vector N,
 * linuxu_rt_inject_irq(N) invokes it with its arg, handler's return
 * is propagated, unregister stops further delivery. */
#include <stdio.h>
#include <stdint.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

extern int rt_irq_register(struct rt_device *dev, int vector,
			   int (*handler)(int irq, void *arg),
			   const char *name, void *arg);
extern void rt_irq_unregister(struct rt_device *dev, int vector);
extern int linuxu_rt_inject_irq(int vector);

static int hits = 0;
static uint64_t last_arg = 0;

static int handler(int irq, void *arg)
{
	hits++;
	last_arg = (uint64_t)(uintptr_t)arg;
	return irq; /* propagate vector */
}

static int handler_two(int irq, void *arg)
{
	(void)irq; (void)arg;
	return 0;
}

int main(void)
{
	int rc = rt_irq_register(NULL, 7, handler, "linuxu-test",
				 (void *)0x1234);
	EXPECT(rc == 0);
	rc = rt_irq_register(NULL, 8, handler_two, "linuxu-test-2",
			     (void *)0x5678);
	EXPECT(rc == 0);
	/* double-register the same vector fails */
	EXPECT(rt_irq_register(NULL, 7, handler, "dup", NULL) != 0);

	/* inject to vector 7 → handler(7, 0x1234) */
	EXPECT(linuxu_rt_inject_irq(7) == 7);
	EXPECT(hits == 1);
	EXPECT(last_arg == 0x1234);

	/* inject to vector 8 → handler_two */
	EXPECT(linuxu_rt_inject_irq(8) == 0);
	EXPECT(hits == 1); /* handler_two does not touch `hits` */

	/* inject to unregistered vector 3 → no-op, 0 */
	EXPECT(linuxu_rt_inject_irq(3) == 0);

	/* unregister vector 7 → inject no longer calls it */
	rt_irq_unregister(NULL, 7);
	hits = 0;
	EXPECT(linuxu_rt_inject_irq(7) == 0);
	EXPECT(hits == 0);
	rt_irq_unregister(NULL, 8);

	/* out-of-range vectors rejected */
	EXPECT(rt_irq_register(NULL, -1, handler, "bad", NULL) != 0);
	fprintf(stderr, "PASS test_irq\n");
	return 0;
}
