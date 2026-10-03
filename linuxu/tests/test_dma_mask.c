/* Linux DMA-mask semantics over both backends.
 *
 * Device masks of 32, 40, 44 and 64 bits (what GMC v6-12 and other drivers
 * ask for) are recorded as requested whenever the platform can satisfy
 * them, and refused with -EIO otherwise.  What the platform can satisfy is
 * the backend's property:
 *  - dext: the DMA seam's answer (dext_dma_platform_supports_bits), modeled
 *    here as a DART that serves widths >= platform_bits; -1 models a seam
 *    that cannot ask the mapper, which falls back to the platform constant.
 *    Each recorded mask publishes its effective width to the seam.
 *  - host: identity addresses are user pointers, so only >= 47 bits fit.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-addr.h>

static const unsigned int widths[] = {32, 40, 44, 64};

#ifdef LINUXU_DEXT_DK
#include <rt/dext_dma.h>

static int platform_bits = 41; /* a DART window at 1 TiB + 2 GiB */
static int platform_known = 1;
static unsigned int published_bits, publishes, probes;

int dext_dma_platform_supports_bits(unsigned int bits)
{
	probes++;
	if (!platform_known)
		return -1;
	return (int)bits >= platform_bits;
}

int dext_dma_set_address_bits(unsigned int bits)
{
	assert(bits >= 32 && bits <= 64);
	published_bits = bits;
	publishes++;
	return 0;
}

static int platform_serves(unsigned int bits)
{
	return platform_known ? (int)bits >= platform_bits : bits >= 42;
}
#else
static int platform_serves(unsigned int bits)
{
	return bits >= 47;
}
#endif

static void reset_device(struct device *dev, u64 *stored)
{
	*stored = DMA_BIT_MASK(32); /* Linux's PCI default */
	*dev = (struct device){0};
	dev->dma_mask = stored;
	dev->coherent_dma_mask = DMA_BIT_MASK(32);
}

static void check_widths(void)
{
	for (unsigned int i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
		const unsigned int bits = widths[i];
		const u64 mask = DMA_BIT_MASK(bits);
		const int expected = platform_serves(bits);
		struct device dev;
		u64 stored;
		int align = 0;

		reset_device(&dev, &stored);
		assert(dma_supported(&dev, mask, &align) == expected);
		assert(align == 14);
#ifdef LINUXU_DEXT_DK
		unsigned int before = publishes;
#endif
		if (expected) {
			/* Recorded exactly as requested, never widened or clamped. */
			assert(dma_set_mask_and_coherent(&dev, mask) == 0);
			assert(stored == mask && dev.coherent_dma_mask == mask);
			assert(dma_get_mask(&dev) == mask);
			assert(!dma_addressing_limited(&dev));
#ifdef LINUXU_DEXT_DK
			assert(publishes == before + 2 && published_bits == bits);
#endif
		} else {
			assert(dma_set_mask_and_coherent(&dev, mask) == -EIO);
			assert(stored == DMA_BIT_MASK(32));
			assert(dev.coherent_dma_mask == DMA_BIT_MASK(32));
			assert(dma_set_coherent_mask(&dev, mask) == -EIO);
			assert(dev.coherent_dma_mask == DMA_BIT_MASK(32));
#ifdef LINUXU_DEXT_DK
			assert(publishes == before);
#endif
		}
		printf("  %2u-bit device mask: %s\n", bits, expected ? "recorded" : "refused (-EIO)");
	}
}

static void check_rules(void)
{
	struct device dev;
	u64 stored;

	reset_device(&dev, &stored);
	/* Not a contiguous low-bit mask, or narrower than Linux's floor. */
	assert(!dma_supported(&dev, 0x123456ULL, NULL));
	assert(!dma_supported(&dev, DMA_BIT_MASK(44) & ~1ULL, NULL));
	assert(!dma_supported(&dev, DMA_BIT_MASK(24), NULL));
	assert(!dma_supported(&dev, 0, NULL));
	assert(dma_set_mask(&dev, DMA_BIT_MASK(24)) == -EIO && stored == DMA_BIT_MASK(32));

	/* A bus limit narrows what the device can reach. */
	assert(dma_set_mask_and_coherent(&dev, DMA_BIT_MASK(64)) == 0);
	dev.bus_dma_limit = DMA_BIT_MASK(32);
	assert(dma_supported(&dev, DMA_BIT_MASK(64), NULL) == platform_serves(32));
	assert(dma_addressing_limited(&dev) == !platform_serves(32));
	dev.bus_dma_limit = 0;
	assert(!dma_addressing_limited(&dev));

	/* A device still at the 32-bit default is limited exactly when the
	 * platform cannot place mappings below 4 GiB. */
	reset_device(&dev, &stored);
	assert(dma_addressing_limited(&dev) == !platform_serves(32));

#ifdef LINUXU_DEXT_DK
	/* The seam receives the narrower of the two masks and the bus limit. */
	if (platform_serves(44)) {
	reset_device(&dev, &stored);
	assert(dma_set_mask(&dev, DMA_BIT_MASK(64)) == 0);
	assert(published_bits == 32); /* coherent mask still at the default */
	assert(dma_set_coherent_mask(&dev, DMA_BIT_MASK(44)) == 0);
	assert(published_bits == 44);
	dev.bus_dma_limit = DMA_BIT_MASK(48);
	assert(dma_set_coherent_mask(&dev, DMA_BIT_MASK(64)) == 0);
	assert(published_bits == 48);
	}
#endif

	/* Without a streaming mask pointer only the coherent mask can be set. */
	reset_device(&dev, &stored);
	dev.dma_mask = NULL;
	assert(dma_get_mask(&dev) == DMA_BIT_MASK(32));
	assert(dma_set_mask(&dev, DMA_BIT_MASK(64)) == -EIO);
	assert(dma_set_coherent_mask(&dev, DMA_BIT_MASK(64)) == 0);
	assert(dev.coherent_dma_mask == DMA_BIT_MASK(64));
	assert(!dma_supported(NULL, DMA_BIT_MASK(64), NULL));
	assert(dma_set_mask(NULL, DMA_BIT_MASK(64)) == -EIO);
	assert(dma_addressing_limited(NULL));
}

int main(void)
{
#ifdef LINUXU_DEXT_DK
	static const struct { int bits, known; const char *name; } platforms[] = {
		{41, 1, "DART window above 1 TiB (41 bits)"},
		{32, 1, "DART window below 4 GiB (32 bits)"},
		{64, 1, "DART that only serves unconstrained commands"},
		{0, 0, "mapper not reachable (platform constant)"},
	};
	for (unsigned int i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
		platform_bits = platforms[i].bits;
		platform_known = platforms[i].known;
		printf("dext, %s:\n", platforms[i].name);
		check_widths();
		check_rules();
	}
	assert(probes);
#else
	printf("host identity backend:\n");
	check_widths();
	check_rules();
#endif
	puts("DMA mask negotiation follows Linux semantics for 32/40/44/64-bit devices");
	return 0;
}
