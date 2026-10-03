/* DMA mask negotiation with Linux semantics for the DriverKit IOVA and host
 * test backends.
 *
 * A device's DMA masks are its own property: dma_set_mask() and
 * dma_set_coherent_mask() record whatever mask the driver asks for (GMC v6-8
 * ask for 40 bits, GMC v9-12 for 44, others for 32 or 64) provided the
 * platform can satisfy it, exactly as Linux does.  dma_supported() answers
 * that question; it never compares against any particular GPU's width.
 *
 * What the platform can satisfy is the platform's limit, not the GPU's:
 *
 *  - DriverKit: every DMA mapping is an IODMACommand translated by the
 *    Apple DART, which hands out IOVAs from a window the SoC's device tree
 *    describes (vm-base/vm-size on the dart node; observed windows range
 *    from below 4 GiB on M1-generation PCIe DARTs to between 1 TiB and
 *    4 TiB on later SoCs).  That node is not in the PCI device's provider
 *    chain, so a dext cannot read it.  Instead the DMA seam asks the mapper
 *    itself (dext_dma_platform_supports_bits): it prepares one small buffer
 *    with maxAddressBits set to the mask's width and checks where the DART
 *    placed it.  Once a mask is recorded, its effective width is handed to
 *    the seam (dext_dma_set_address_bits), which creates every later
 *    IODMACommand with that maxAddressBits and refuses mappings above it.
 *  - Host test backend: identity IOVAs are user-space pointers, which lie
 *    below 2^47 on arm64 and x86-64 macOS, so only masks that wide fit.
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-addr.h>
#include <linux/errno.h>

/* Linux's floor: no PCI DMA mask narrower than 32 bits is supported. */
#define LINUXU_DMA_MIN_MASK DMA_BIT_MASK(32)

#ifdef LINUXU_DEXT_DK
#include <rt/dext_dma.h>

/* Used only when the mapper cannot be asked (no PCI provider yet, or the
 * probe failed for an unrelated reason): a width that covers every DART
 * IOVA window observed on Apple Silicon.  A platform limit, not a GPU one. */
#define LINUXU_DMA_PLATFORM_FALLBACK_BITS 42u

static bool dma_platform_can_address(unsigned int bits)
{
	int answer = dext_dma_platform_supports_bits(bits);

	if (answer >= 0)
		return answer != 0;
	return bits >= LINUXU_DMA_PLATFORM_FALLBACK_BITS;
}
#else
#define LINUXU_DMA_HOST_ADDRESS_BITS 47u

static bool dma_platform_can_address(unsigned int bits)
{
	return bits >= LINUXU_DMA_HOST_ADDRESS_BITS;
}
#endif

/* Widest n such that every address below 2^n is within `limit`. */
static unsigned int dma_limit_bits(u64 limit)
{
	if (limit == ~0ULL)
		return 64;
	return 63u - (unsigned int)__builtin_clzll(limit + 1);
}

static u64 dma_min_limit(u64 mask, u64 limit)
{
	return limit && limit < mask ? limit : mask;
}

u64 dma_get_mask(struct device *dev)
{
	if (dev && dev->dma_mask && *dev->dma_mask)
		return *dev->dma_mask;
	return DMA_BIT_MASK(32);
}

/* The narrowest constraint any mapping for this device must satisfy: both
 * masks (the seam serves streaming and coherent mappings alike) and the
 * bus limit. */
static u64 dma_effective_mask(struct device *dev)
{
	u64 mask = dma_get_mask(dev);

	if (dev->coherent_dma_mask && dev->coherent_dma_mask < mask)
		mask = dev->coherent_dma_mask;
	return dma_min_limit(mask, dev->bus_dma_limit);
}

static void dma_publish_width(struct device *dev)
{
#ifdef LINUXU_DEXT_DK
	/* A dext binds exactly one PCI function; its masks set the width of
	 * every DART mapping the seam creates from now on. */
	(void)dext_dma_set_address_bits(dma_limit_bits(dma_effective_mask(dev)));
#else
	(void)dev;
#endif
}

bool dma_supported(struct device *dev, u64 mask, int *align)
{
	if (align)
		*align = 14; /* DriverKit allocations use 16 KiB host pages. */
	if (!dev || !mask)
		return false;
	/* DMA masks describe a contiguous range of low address bits. */
	if ((mask & (mask + 1)) != 0 || mask < LINUXU_DMA_MIN_MASK)
		return false;
	return dma_platform_can_address(dma_limit_bits(dma_min_limit(mask, dev->bus_dma_limit)));
}

int dma_set_mask(struct device *dev, u64 mask)
{
	if (!dev || !dev->dma_mask || !dma_supported(dev, mask, NULL))
		return -EIO;
	*dev->dma_mask = mask;
	dma_publish_width(dev);
	return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (!dma_supported(dev, mask, NULL))
		return -EIO;
	dev->coherent_dma_mask = mask;
	dma_publish_width(dev);
	return 0;
}

int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	int ret = dma_set_mask(dev, mask);
	if (ret)
		return ret;
	return dma_set_coherent_mask(dev, mask);
}

/* As with a Linux IOMMU, any page of system memory is remapped below the
 * device's mask, so a device is addressing-limited only when the platform
 * cannot satisfy its effective mask at all (TTM then falls back to DMA32
 * pools). */
bool dma_addressing_limited(struct device *dev)
{
	u64 mask;

	if (!dev)
		return true;
	mask = dma_min_limit(dma_get_mask(dev), dev->bus_dma_limit);
	return mask < LINUXU_DMA_MIN_MASK || !dma_platform_can_address(dma_limit_bits(mask));
}
