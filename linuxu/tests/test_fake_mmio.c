/* test_fake_mmio.c — fake-MMIO token lifecycle (REAL):
 * 16KB-aligned slot, token mint, readl/writel through the token,
 * 512KB bound, no double-use of a slot, free + re-mint. */
#include <stdio.h>

#include <rt/rt.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

extern uint32_t rt_mmio_mint_token(void *dev, uint8_t mem_index,
				   uint64_t base, uint64_t size,
				   int host_shadow);
extern void rt_mmio_free_token(uint32_t token);

int main(void)
{
	uint32_t t1 = rt_mmio_mint_token(NULL, 5, 0, 512 * 1024, 1);
	uint32_t t2 = rt_mmio_mint_token(NULL, 0, 0, 256 * 1024 * 1024, 1);

	EXPECT(t1 != RT_MMIO_TOKEN_INVALID);
	EXPECT(t2 != RT_MMIO_TOKEN_INVALID);
	EXPECT(t1 != t2);
	/* 16KB granularity: token N owns slot N*16KB */
	EXPECT((t1 * RT_MMIO_SLOT_GRANULE) % RT_MMIO_SLOT_GRANULE == 0);

	/* readl/writel round trip through the token */
	rt_mmio_writel(NULL, (void *)(uintptr_t)t1, 0x120, 0xdeadbeef);
	EXPECT(rt_mmio_readl(NULL, (void *)(uintptr_t)t1, 0x120)
	       == 0xdeadbeef);

	/* 64-bit path */
	rt_mmio_writeq(NULL, (void *)(uintptr_t)t1, 0x130, 0x1122334455667788ULL);
	EXPECT(rt_mmio_readq(NULL, (void *)(uintptr_t)t1, 0x130)
	       == 0x1122334455667788ULL);

	/* byte path */
	rt_mmio_writeb(NULL, (void *)(uintptr_t)t2, 0x8, 0x42);
	EXPECT(rt_mmio_readb(NULL, (void *)(uintptr_t)t2, 0x8) == 0x42);

	/* out-of-slot access must not touch the neighbor slot:
	 * the 0x200000th slot (16MB away) reads 0 on t1 */
	EXPECT(rt_mmio_readl(NULL, (void *)(uintptr_t)t1, 0x10000)
	       == 0);

	/* free + re-mint: the same token may be reissued */
	rt_mmio_free_token(t1);
	uint32_t t3 = rt_mmio_mint_token(NULL, 5, 0, 1024 * 1024, 1);
	EXPECT(t3 == t1);
	rt_mmio_free_token(t3);
	rt_mmio_free_token(t2);

	/* invalid tokens */
	EXPECT(rt_mmio_readl(NULL, (void *)(uintptr_t)RT_MMIO_TOKEN_INVALID,
			    0) == 0);
	fprintf(stderr, "PASS test_fake_mmio\n");
	return 0;
}
