/* Kernel random numbers (linux/random.h) from the platform CSPRNG. */
#include <stdlib.h>

#include <linux/types.h>
#include <linux/random.h>

void get_random_bytes(void *buf, size_t len)
{
	arc4random_buf(buf, len);
}

u8 get_random_u8(void)
{
	u8 v;

	arc4random_buf(&v, sizeof(v));
	return v;
}

u16 get_random_u16(void)
{
	u16 v;

	arc4random_buf(&v, sizeof(v));
	return v;
}

u32 get_random_u32(void)
{
	return arc4random();
}

u64 get_random_u64(void)
{
	u64 v;

	arc4random_buf(&v, sizeof(v));
	return v;
}

u32 __get_random_u32_below(u32 ceil)
{
	/* arc4random_uniform() rejects the biased range like Linux does. */
	return ceil ? arc4random_uniform(ceil) : 0;
}
