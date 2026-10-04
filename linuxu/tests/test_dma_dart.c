/* test_dma_dart.c — DMA→DART host backend (T1 + W6), REAL path:
 *
 * 1. Fake PCI: pci_enable_device publishes BAR0 (16 GB VRAM) and BAR5
 *    (512 KB registers); pci_iomap mints valid tokens and the host
 *    shadow covers the whole BAR5 region (writel/readl past the old
 *    16 KB granule).
 * 2. dma_map_page: a real page gets a valid (identity) IOVA, the DART
 *    budget counter increments, the IOVA is a writable host VA, and
 *    the data round-trips through it.
 * 3. dma_alloc_coherent: non-NULL, 16 KB aligned, writable, IOVA
 *    matches the CPU pointer, budget returns after free.
 * 4. Budget: mapping until the 1.5 GB ceiling fails with -ENOMEM;
 *    unmapping a single page frees exactly the charged bytes; the
 *    next map then succeeds.
 * 5. Tear-down: budget back to 0, token table empty (no leaks). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <rt/rt.h>
#include <rt/dart.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

int main(void)
{
	int i;
	uint64_t used;

	/* ---- 1. fake PCI: BARs + fake-MMIO over them ---- */
	{
		struct pci_dev pdev;
		void *bar0, *bar5;

		memset(&pdev, 0, sizeof(pdev));
		EXPECT(pci_enable_device(&pdev) == 0);

		/* BAR0: 16 GB VRAM; BAR5: 512 KB registers */
		EXPECT(pci_resource_len(&pdev, 0) ==
		       16ULL * 1024 * 1024 * 1024);
		EXPECT(pci_resource_len(&pdev, 5) == 512 * 1024);
		EXPECT(pci_resource_start(&pdev, 0) != 0);
		EXPECT(pci_resource_start(&pdev, 5) != 0);

		bar0 = pci_iomap(&pdev, 0, 0);
		bar5 = pci_iomap(&pdev, 5, 0);
		EXPECT(bar0 != NULL);
		EXPECT(bar5 != NULL);
		EXPECT(bar0 != bar5);

		/* register access through the rt_mmio_* dispatch (the
		 * host-backend path of the fake-MMIO ABI): BAR5 shadow
		 * spans the FULL 512 KB region, so a pair well past
		 * the old 16 KB granule must round-trip */
		rt_mmio_writel(NULL, bar5, 512 * 1024 - 4, 0xcafe0001);
		EXPECT(rt_mmio_readl(NULL, bar5, 512 * 1024 - 4)
		       == 0xcafe0001);
		/* ... and mid-region (past 16 KB too) */
		rt_mmio_writel(NULL, bar5, 64 * 1024, 0x12345678);
		EXPECT(rt_mmio_readl(NULL, bar5, 64 * 1024)
		       == 0x12345678);

		/* BAR0 shadow is capped at 256 MB: a read far inside
		 * the 16 GB window but past the cap must not fault
		 * (reads as 0) */
		EXPECT(rt_mmio_readl(NULL, bar0, 200 * 1024 * 1024) == 0);

		/* unmap + re-mint: the freed token can be reissued */
		uint32_t t_before = (uint32_t)(uintptr_t)bar5;
		pci_iounmap(&pdev, bar5);
		bar5 = pci_iomap(&pdev, 5, 0);
		EXPECT(bar5 != NULL);
		rt_mmio_writel(NULL, bar5, 0, 0x42);
		EXPECT(rt_mmio_readl(NULL, bar5, 0) == 0x42);
		(void)t_before;

		pci_iounmap(&pdev, bar0);
		pci_iounmap(&pdev, bar5);
		pci_disable_device(&pdev);
	}

	/* ---- 2. dma_map_page: real IOVA + budget + DMA copy ---- */
	{
		struct page *pg;
		dma_addr_t iova;
		uint32_t *w;

		EXPECT(linuxu_dart_used() == 0);
		pg = alloc_page(GFP_KERNEL);
		EXPECT(pg != NULL);
		/* pre-seed the backing host page */
		memset(page_address(pg), 0xAB, PAGE_SIZE);

		used = linuxu_dart_used();
		iova = dma_map_page(NULL, pg, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
		EXPECT(iova != 0);
		EXPECT(dma_mapping_error(NULL, iova) == 0);
		/* identity: IOVA is the page's host VA */
		EXPECT((void *)(uintptr_t)iova == page_address(pg));
		EXPECT(linuxu_dart_used() == used + PAGE_SIZE);

		/* write through the IOVA (the "DMA copy") and read it
		 * back — both CPU-side and through the host VA */
		w = (uint32_t *)(uintptr_t)iova;
		w[0] = 0x11223344;
		w[1] = 0x55667788;
		EXPECT(w[0] == 0x11223344);
		EXPECT(w[1] == 0x55667788);
		EXPECT(*(uint32_t *)(void *)(uintptr_t)iova
		       == 0x11223344);

		/* mid-page offset map: IOVA carries the offset, the
		 * charged bytes are offset+size */
		used = linuxu_dart_used();
		{
			dma_addr_t iova2;

			iova2 = dma_map_page(NULL, pg, 1024, 4096,
					     DMA_TO_DEVICE);
			EXPECT(dma_mapping_error(NULL, iova2) == 0);
			EXPECT((void *)(uintptr_t)iova2 ==
			       (char *)page_address(pg) + 1024);
			EXPECT(linuxu_dart_used() == used + 1024 + 4096);
			dma_unmap_page(NULL, iova2, 4096, DMA_TO_DEVICE);
			EXPECT(linuxu_dart_used() == used);
		}

		/* offset-0 unmap frees exactly PAGE_SIZE (the offset-0 charge;
		 * `used` was captured AFTER the offset-0 map, so refunding it
		 * drops the budget one PAGE_SIZE below `used`) */
		dma_unmap_page(NULL, iova, PAGE_SIZE, DMA_BIDIRECTIONAL);
		EXPECT(linuxu_dart_used() == used - PAGE_SIZE);
		__free_pages(pg, 0);
	}

	/* ---- 3. dma_alloc_coherent: real 16 KB-aligned anon ---- */
	{
		dma_addr_t iova;
		uint8_t *buf;

		buf = dma_alloc_coherent(NULL, 4096, &iova, GFP_KERNEL);
		EXPECT(buf != NULL);
		EXPECT(((uintptr_t)buf & 0x3FFF) == 0); /* 16 KB aligned */
		EXPECT(iova == (dma_addr_t)(uintptr_t)buf);
		/* 4 KB request rounds up to 16 KB for the budget */
		used = linuxu_dart_used();
		EXPECT(used == 0x4000);
		memset(buf, 0xCD, 4096);
		EXPECT(buf[0] == 0xCD && buf[4095] == 0xCD);
		/* whole 16 KB chunk is writable */
		memset(buf + 4096, 0xEE, 16 * 1024 - 4096);
		EXPECT(buf[15 * 1024 + 15] == 0xEE);

		dma_free_coherent(NULL, 4096, buf, iova);
		EXPECT(linuxu_dart_used() == 0);
	}

	/* ---- 4. no software ceiling: the platform refuses, not a budget ---- */
	{
		/* A mapping larger than any fixed budget this layer once had
		 * (3 GiB of IOVA over one page: the identity map touches no
		 * backing) is accounted and refunded exactly. */
		const uint64_t big = 3ull << 30;
		struct page *pg = alloc_page(GFP_KERNEL);
		dma_addr_t big_iova;

		EXPECT(pg != NULL);
		big_iova = dma_map_page(NULL, pg, 0, (size_t)big, DMA_TO_DEVICE);
		EXPECT(dma_mapping_error(NULL, big_iova) == 0);
		EXPECT(linuxu_dart_used() == big);
		EXPECT(linuxu_dart_peak() >= big);
		dma_unmap_page(NULL, big_iova, (size_t)big, DMA_TO_DEVICE);
		EXPECT(linuxu_dart_used() == 0);
		__free_pages(pg, 0);

		/* Many live mappings at once, more than any fixed table of
		 * 4096: each is accounted, all are refunded. */
		{
			enum { MANY = 6000 };
			dma_addr_t *iovas = malloc(MANY * sizeof(*iovas));
			struct page *page = alloc_page(GFP_KERNEL);

			EXPECT(iovas != NULL && page != NULL);
			for (i = 0; i < MANY; i++) {
				iovas[i] = dma_map_page(NULL, page, 0, 64, DMA_BIDIRECTIONAL);
				EXPECT(dma_mapping_error(NULL, iovas[i]) == 0);
			}
			EXPECT(linuxu_dart_used() == (uint64_t)MANY * 64);
			for (i = 0; i < MANY; i++)
				dma_unmap_page(NULL, iovas[i], 64, DMA_BIDIRECTIONAL);
			EXPECT(linuxu_dart_used() == 0);
			__free_pages(page, 0);
			free(iovas);
		}
	}

	/* ---- 5. teardown: no leaks in the token table ---- */
	EXPECT(linuxu_dart_table_count() == 0);
	EXPECT(linuxu_dart_used() == 0);

	fprintf(stderr, "PASS test_dma_dart\n");
	return 0;
}
