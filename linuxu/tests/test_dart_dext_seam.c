/* test_dart_dext_seam.c — DART/DMA budget + coherent contract
 * (T-dma-dart-dext), host backend (no DriverKit).
 *
 * Host-verifiable half of the DART track: the linuxu DART layer
 * (linuxu/src/dart/dart.c) budget + coherent round-trip contract that the
 * new dext hook preserves.  On the host build the coherent path uses
 * identity IOVA (host VA == IOVA, the in-process model) — byte-identical to
 * before the dext hook landed (LINUXU_DEXT_DK is not defined on host).
 * The dext build's real IODMACommand path (a DART-assigned IOVA) is the
 * same contract, verified on hardware by the p3-hw-gate.
 *
 *   1. coherent alloc: non-NULL, 16 KB-aligned, identity IOVA, writable.
 *   2. the 1.5 GB DART budget charged + refunded exactly.
 *   3. 16 KB minimum: a 1-byte request still gets a 16 KB buffer.
 *   4. tear-down: budget back to 0, token table empty (no leaks). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <linux/dma-mapping.h>
#include <rt/dart.h>

#define ALIGN16 (0x4000UL)
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
	uint64_t used_before, used_after;
	void *cpu;
	dma_addr_t iova;

	/* ---- 1. coherent alloc: non-NULL, 16 KB aligned, identity IOVA,
	 * writable ---- */
	{
		size_t size = 3 * 1024 + 7; /* deliberately unaligned */
		cpu = dma_alloc_coherent(NULL, size, &iova, 0);
		EXPECT(cpu != NULL);
		EXPECT(((uintptr_t)cpu % ALIGN16) == 0); /* 16 KB aligned */
		/* identity IOVA: the GPU-visible address == the host ptr */
		EXPECT(iova == (dma_addr_t)(uintptr_t)cpu);
		/* writable + usable for at least the requested size */
		memset(cpu, 0xA5, size);
		EXPECT(((volatile uint8_t *)cpu)[0] == 0xA5);
		EXPECT(((volatile uint8_t *)cpu)[size - 1] == 0xA5);
		dma_free_coherent(NULL, size, cpu, iova);
	}

	/* ---- 2. the DART budget charged + refunded exactly ---- */
	{
		size_t size = 256 * 1024; /* 256 KB -> rounds to 256 KB */
		used_before = linuxu_dart_used();
		cpu = dma_alloc_coherent(NULL, size, &iova, 0);
		EXPECT(cpu != NULL);
		used_after = linuxu_dart_used();
		EXPECT(used_after == used_before + size); /* exact charge */
		dma_free_coherent(NULL, size, cpu, iova);
		used_after = linuxu_dart_used();
		EXPECT(used_after == used_before); /* exact refund */
	}

	/* ---- 3. 16 KB minimum: a 1-byte request still gets 16 KB ---- */
	{
		used_before = linuxu_dart_used();
		cpu = dma_alloc_coherent(NULL, 1, &iova, 0);
		EXPECT(cpu != NULL);
		EXPECT(((uintptr_t)cpu % ALIGN16) == 0);
		used_after = linuxu_dart_used();
		EXPECT(used_after == used_before + ALIGN16); /* 16 KB min */
		dma_free_coherent(NULL, 1, cpu, iova);
	}

	/* ---- 4. tear-down: budget back to 0, no token-table leaks ---- */
	EXPECT(linuxu_dart_used() == 0);
	EXPECT(linuxu_dart_table_count() == 0);

	fprintf(stderr, "test_dart_dext_seam: all checks passed\n");
	return 0;
}
