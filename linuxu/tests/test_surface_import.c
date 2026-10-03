/* Surface import offline (rt/surface.h): memory the "platform" mapped for
 * the device, given as DMA segments, imported as an amdgpu dma-buf (an SG
 * BO bound in the GART) and copied into a VRAM buffer by the SDMA buffer
 * functions, against the fixture device of cs_fixture.c, whose software
 * SDMA engine reads system memory through the GART table upstream filled
 * and faults on anything the DART does not map. Host DMA addresses are
 * host pointers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int usleep(unsigned int usec);

#include <linux/pci.h>
#include <drm/drm_device.h>
#include <rt/dart.h>
#include <rt/surface.h>
#include <rt/removal.h>

#include "amdgpu.h"
#include "cs_fixture.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

#define W	640u
#define H	360u
#define SRC_PITCH	2816u	/* not the VRAM buffer's pitch */
#define DST_PITCH	(W * 4)
#define PAGE	(16u * 1024u)

/* A surface of three separately allocated runs: the GART sees one buffer,
 * the "platform" three DMA segments. */
struct fake_surface {
	uint8_t *run[3];
	uint64_t run_bytes[3];
	uint64_t size;
	struct rt_surface_segment segment[3];
	int releases;
};

static uint8_t *surface_byte(struct fake_surface *s, uint64_t offset)
{
	for (int i = 0; i < 3; i++) {
		if (offset < s->run_bytes[i])
			return s->run[i] + offset;
		offset -= s->run_bytes[i];
	}
	return NULL;
}

static uint32_t pixel_value(uint32_t x, uint32_t y, uint32_t salt)
{
	return (x << 12) ^ (y << 1) ^ salt;
}

static void fake_surface_init(struct fake_surface *s, uint32_t pitch, uint32_t salt)
{
	uint64_t pages = ((uint64_t)pitch * H + PAGE - 1) / PAGE;
	const uint64_t split[3] = { pages / 3, pages / 3, pages - 2 * (pages / 3) };

	memset(s, 0, sizeof(*s));
	s->size = pages * PAGE;
	for (int i = 0; i < 3; i++) {
		s->run_bytes[i] = split[i] * PAGE;
		s->run[i] = aligned_alloc(PAGE, s->run_bytes[i]);
		CHECK(s->run[i]);
		memset(s->run[i], 0xee, s->run_bytes[i]);
		s->segment[i] = (struct rt_surface_segment){ (uint64_t)(uintptr_t)s->run[i],
							     s->run_bytes[i] };
	}
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v = pixel_value(x, y, salt);

			memcpy(surface_byte(s, (uint64_t)y * pitch + x * 4), &v, 4);
		}
}

static void fake_surface_free(struct fake_surface *s)
{
	for (int i = 0; i < 3; i++)
		free(s->run[i]);
}

static void provider_release(void *context)
{
	((struct fake_surface *)context)->releases++;
}

static void wait_released(struct fake_surface *s)
{
	/* TTM may destroy a busy BO later, from its delayed-delete work. */
	for (int i = 0; i < 5000 && !s->releases; i++)
		usleep(1000);
	CHECK(s->releases == 1);
	for (int i = 0; i < 3; i++)
		CHECK(!linuxu_dart_contains(s->segment[i].dma_address, PAGE));
}

/* Pixels inside @rects match the surface; outside they are untouched (0). */
static void check_copy(const uint8_t *vram, const struct rt_surface_rect *rects, int n,
		       uint32_t salt)
{
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v;
			int inside = 0;

			memcpy(&v, vram + (uint64_t)y * DST_PITCH + x * 4, 4);
			for (int i = 0; i < n; i++)
				inside |= x >= rects[i].x && x < rects[i].x + rects[i].width &&
					  y >= rects[i].y && y < rects[i].y + rects[i].height;
			if (v != (inside ? pixel_value(x, y, salt) : 0)) {
				fprintf(stderr, "pixel %u,%u: 0x%08x (inside %d)\n", x, y, v, inside);
				abort();
			}
		}
}

int main(void)
{
	struct pci_dev *pdev = cs_fixture_init();
	struct amdgpu_device *adev = drm_to_adev(pci_get_drvdata(pdev));
	struct fake_surface surf, same;
	struct rt_surface_provider provider = { provider_release, &surf };
	struct rt_surface_copy_stats stats;
	struct cs_fixture_stats before, after;
	struct rt_surface *surface;
	struct amdgpu_bo *dst = NULL;
	uint64_t dst_gpu = 0;
	void *dst_cpu = NULL;
	int r;

	fake_surface_init(&surf, SRC_PITCH, 0xa5000000u);

	/* Refusals: nothing is imported, the provider is not released, nothing
	 * stays charged. */
	{
		struct rt_surface_segment bad[3];
		uint64_t used = linuxu_dart_used();

		memcpy(bad, surf.segment, sizeof(bad));
		bad[1].dma_address += 4096;
		CHECK(rt_surface_import(pdev, bad, 3, surf.size, W, H, SRC_PITCH, &provider,
					&surface) == -EINVAL);
		CHECK(rt_surface_import(pdev, surf.segment, 3, surf.size - PAGE, W, H, SRC_PITCH,
					&provider, &surface) == -EINVAL);
		CHECK(rt_surface_import(pdev, surf.segment, 3, surf.size, W, H, W * 4 - 4, &provider,
					&surface) == -EINVAL);
		CHECK(rt_surface_import(pdev, surf.segment, 3, surf.size, W, H * 4, SRC_PITCH,
					&provider, &surface) == -EINVAL);
		CHECK(!surf.releases && linuxu_dart_used() == used);
	}

	r = rt_surface_import(pdev, surf.segment, 3, surf.size, W, H, SRC_PITCH, &provider, &surface);
	printf("surface: import -> %d, %llu bytes in 3 segments at GART 0x%llx\n", r,
	       (unsigned long long)surf.size, (unsigned long long)rt_surface_gpu_address(surface));
	CHECK(r == 0 && surface);
	CHECK(rt_surface_gpu_address(surface) >= adev->gmc.gart_start &&
	      rt_surface_gpu_address(surface) + surf.size - 1 <= adev->gmc.gart_end);
	for (int i = 0; i < 3; i++)
		CHECK(linuxu_dart_contains(surf.segment[i].dma_address, surf.segment[i].length));

	/* The scanout-like destination in VRAM. */
	CHECK(amdgpu_bo_create_kernel(adev, (uint64_t)DST_PITCH * H, PAGE, AMDGPU_GEM_DOMAIN_VRAM,
				      &dst, &dst_gpu, &dst_cpu) == 0);
	memset(dst_cpu, 0, (uint64_t)DST_PITCH * H);

	/* The whole frame, pitches differing: one copy per row. */
	{
		const struct rt_surface_rect all = { 0, 0, W, H };

		cs_fixture_stats(&before);
		r = rt_surface_copy(surface, &dst->tbo.base, DST_PITCH, &all, 1, 5000, &stats);
		cs_fixture_stats(&after);
		printf("surface: full copy -> %d, %u SDMA job(s), %u rows, %llu bytes, %llu us\n", r,
		       stats.jobs, stats.rows, (unsigned long long)stats.bytes,
		       (unsigned long long)stats.ns / 1000);
		/* 360 row copies in one job. */
		CHECK(r == 0 && stats.jobs == 1 && stats.rows == H &&
		      stats.bytes == (uint64_t)W * 4 * H);
		CHECK(after.copies - before.copies == H && after.sdma_ibs - before.sdma_ibs == 1);
		CHECK(after.faults == 0 && after.dart_faults == 0);
		check_copy(dst_cpu, &all, 1, 0xa5000000u);
	}

	/* Damage only: two rectangles, one clipped at the edge. */
	{
		const struct rt_surface_rect rects[2] = { { 100, 50, 200, 100 }, { 600, 340, 100, 100 } };
		const struct rt_surface_rect clipped[2] = { { 100, 50, 200, 100 }, { 600, 340, 40, 20 } };

		memset(dst_cpu, 0, (uint64_t)DST_PITCH * H);
		r = rt_surface_copy(surface, &dst->tbo.base, DST_PITCH, rects, 2, 5000, &stats);
		printf("surface: damage copy -> %d, %u SDMA job(s), %llu bytes\n", r, stats.jobs,
		       (unsigned long long)stats.bytes);
		CHECK(r == 0 && stats.jobs == 1 && stats.rows == 120 &&
		      stats.bytes == 200 * 4 * 100 + 40 * 4 * 20);
		check_copy(dst_cpu, clipped, 2, 0xa5000000u);
	}

	/* Bad destinations are refused before any copy. */
	CHECK(rt_surface_copy(surface, &dst->tbo.base, W * 4 - 4, (struct rt_surface_rect[]){ { 0, 0, 1, 1 } },
			      1, 5000, &stats) == -EINVAL);
	CHECK(rt_surface_copy(surface, &dst->tbo.base, DST_PITCH * 2, (struct rt_surface_rect[]){ { 0, 0, 1, 1 } },
			      1, 5000, &stats) == -EINVAL);

	/* More rows than one job carries: two jobs, in order. */
	{
		struct rt_surface_rect rows[1100];

		memset(dst_cpu, 0, (uint64_t)DST_PITCH * H);
		for (int i = 0; i < 1100; i++)
			rows[i] = (struct rt_surface_rect){ (uint32_t)(i % 4) * 160, (uint32_t)i % H, 1, 1 };
		r = rt_surface_copy(surface, &dst->tbo.base, DST_PITCH, rows, 1100, 5000, &stats);
		printf("surface: 1100 one-pixel rects -> %d, %u SDMA job(s)\n", r, stats.jobs);
		CHECK(r == 0 && stats.jobs == 2 && stats.rows == 1100 && stats.bytes == 1100 * 4);
	}

	/* The pinning check: the GPU (SDMA into GTT) and a CPU view see the
	 * pattern the client wrote last, and not the one before. */
	{
		struct rt_surface_verify_result v;
		uint8_t *view = malloc(surf.size);

		CHECK(view);
		for (int seed = 1; seed <= 3; seed++) {
			for (uint64_t d = 0; d < surf.size / 4; d++) {
				uint32_t value = rt_surface_pattern(seed, d);

				memcpy(surface_byte(&surf, d * 4), &value, 4);
				memcpy(view + d * 4, &value, 4);
			}
			CHECK(rt_surface_verify(surface, seed, view, 5000, &v) == 0);
			printf("surface: verify seed %d -> GPU %u, CPU %u mismatching dwords of %u, %llu us\n",
			       seed, v.gpu_mismatches, v.cpu_mismatches,
			       v.samples * v.sample_bytes / 4, (unsigned long long)v.gpu_ns / 1000);
			CHECK(v.version == 1 && v.samples == RT_SURFACE_SAMPLES && v.cpu_checked);
			CHECK(!v.gpu_mismatches && !v.cpu_mismatches && v.first_gpu_mismatch == UINT64_MAX);
			CHECK(v.gpu_value == v.expected_value && v.gpu_address == rt_surface_gpu_address(surface));
			CHECK(rt_surface_verify(surface, seed + 100, NULL, 5000, &v) == 0);
			CHECK(v.gpu_mismatches == v.samples * v.sample_bytes / 4 && !v.cpu_checked &&
			      v.first_gpu_mismatch == 0);
		}
		free(view);
	}

	/* The import table: handles by owner, removal by handle, by owner and
	 * all (the client-stop and session-close paths). */
	{
		struct fake_surface t[3];
		struct rt_surface_provider p[3];
		struct rt_surface *imported[3];
		uint32_t h[3];

		for (int i = 0; i < 3; i++) {
			fake_surface_init(&t[i], SRC_PITCH, 0x11000000u * (i + 1));
			p[i] = (struct rt_surface_provider){ provider_release, &t[i] };
			CHECK(rt_surface_import(pdev, t[i].segment, 3, t[i].size, W, H, SRC_PITCH, &p[i],
						&imported[i]) == 0);
			CHECK(rt_surface_provider_context(imported[i]) == &t[i]);
		}
		h[0] = rt_surface_add(7, imported[0]);
		h[1] = rt_surface_add(7, imported[1]);
		h[2] = rt_surface_add(9, imported[2]);
		CHECK(h[0] && h[1] && h[2] && h[0] != h[1] && rt_surface_count() == 3);
		CHECK(rt_surface_get(7, h[0]) == imported[0] && !rt_surface_get(9, h[0]) &&
		      !rt_surface_get(7, 0));
		CHECK(rt_surface_remove(9, h[0]) == -ENOENT && rt_surface_remove(7, 0) == -ENOENT);
		CHECK(rt_surface_remove(7, h[0]) == 0 && !rt_surface_get(7, h[0]));
		wait_released(&t[0]);
		CHECK(rt_surface_remove_owner(7) == 1 && rt_surface_count() == 1);
		wait_released(&t[1]);
		CHECK(rt_surface_remove_all() == 1 && rt_surface_count() == 0);
		wait_released(&t[2]);
		for (int i = 0; i < 3; i++)
			fake_surface_free(&t[i]);
	}

	/* Release: the provider's release follows once the BO is gone, the
	 * DART charge with it. */
	rt_surface_release(surface);
	wait_released(&surf);
	printf("surface: released, provider released once\n");

	/* Equal pitches: whole rows are one range. */
	fake_surface_init(&same, DST_PITCH, 0x3c000000u);
	provider.context = &same;
	CHECK(rt_surface_import(pdev, same.segment, 3, same.size, W, H, DST_PITCH, &provider,
				&surface) == 0);
	{
		const struct rt_surface_rect band = { 0, 10, W, 200 };

		memset(dst_cpu, 0, (uint64_t)DST_PITCH * H);
		cs_fixture_stats(&before);
		r = rt_surface_copy(surface, &dst->tbo.base, DST_PITCH, &band, 1, 5000, &stats);
		cs_fixture_stats(&after);
		printf("surface: band copy (equal pitch) -> %d, %lu copy packet(s)\n", r,
		       after.copies - before.copies);
		CHECK(r == 0 && stats.jobs == 1 && after.copies - before.copies == 1 &&
		      stats.bytes == (uint64_t)DST_PITCH * 200);
		check_copy(dst_cpu, &band, 1, 0x3c000000u);
	}
	rt_surface_release(surface);
	wait_released(&same);
	CHECK(surf.releases == 1);

	amdgpu_bo_free_kernel(&dst, &dst_gpu, &dst_cpu);
	cs_fixture_stats(&after);
	CHECK(after.faults == 0 && after.dart_faults == 0);

	/* The GPU leaves the bus while an import is held: nothing reaches the
	 * GPU any more, and releasing still returns the client's mapping. */
	{
		struct fake_surface held;
		struct rt_surface_verify_result v;
		struct rt_surface *other = NULL;

		fake_surface_init(&held, SRC_PITCH, 0x77000000u);
		provider.context = &held;
		CHECK(rt_surface_import(pdev, held.segment, 3, held.size, W, H, SRC_PITCH, &provider,
					&surface) == 0);
		CHECK(rt_surface_add(11, surface));
		CHECK(rt_removal_begin(pdev) == 0);
		CHECK(rt_surface_verify(surface, 0x77000000u, NULL, 1000, &v) == -ENODEV);
		CHECK(rt_surface_import(pdev, held.segment, 3, held.size, W, H, SRC_PITCH, &provider,
					&other) == -ENODEV && !other);
		CHECK(rt_surface_remove_all() == 1);
		wait_released(&held);
		rt_removal_end();
		printf("surface: removal with an import held: checks refused, the import released\n");
		fake_surface_free(&held);
	}
	cs_fixture_stop();
	fake_surface_free(&surf);
	fake_surface_free(&same);
	printf("PASS surface import: DMA segments as an amdgpu dma-buf in the GART, SDMA copies of "
	       "whole frames, damage and bands into VRAM, refusals, release after the BO, removal\n");
	return 0;
}
