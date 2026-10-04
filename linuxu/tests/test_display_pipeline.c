/* The display output pipeline offline (rt/display.h, rt/surface.h): the CS
 * fixture device (GART, VM, rings, two software SDMA engines that execute
 * what upstream submits) with the fixture DCN 4.0.1 display of
 * test-dm-offline on it, the unmodified amdgpu_dm and Display Core.
 *
 * A client surface (three host allocations as DMA segments) is imported,
 * an output is lit on a sink, and frames are presented: each PRESENT only
 * queues; the output's worker copies with SDMA into one of three
 * framebuffers and flips with a nonblocking commit whose out-fence the
 * fixture's "vblank" signals: a thread that raises the HUBP flip
 * interrupts through amdgpu's interrupt source, as the IH would, every
 * 16 ms. Checked: the scanned-out buffer holds the frame; damage-only
 * copies; frames replaced in the mailbox while no flip can happen; a whole
 * frame split over both SDMA engines; latency and counters; the output
 * stopped and the screen restored. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int usleep(unsigned int usec);

#include <linux/pci.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_mode_config.h>
#include <rt/dart.h>
#include <rt/display.h>
#include <rt/device_string.h>
#include <rt/removal.h>
#include <rt/surface.h>

#include "amdgpu.h"
#include "amdgpu_dm.h"
#include "dcn/dcn_4_1_0_offset.h"
#include "ivsrcid/dcn/irqsrcs_dcn_1_0.h"
#include "soc15_ih_clientid.h"
#include "cs_fixture.h"
#include "dcn401_fixture.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

extern const struct amdgpu_ip_block_version dm_ip_block;
extern int fw_table_register_embedded(void);
extern int drm_edid_override_set(struct drm_connector *connector, const void *edid, size_t size);
extern u64 ktime_get_ns(void);

#define W	1920u
#define H	1080u
#define PITCH	(W * 4)
#define PAGE	(16u * 1024u)

static struct amdgpu_device *adev;

static void display_init(struct amdgpu_device *a)
{
	struct amdgpu_ip_block block = { .adev = a, .version = &dm_ip_block };

	drm_mode_config_init(adev_to_drm(a));
	a->firmware.load_type = AMDGPU_FW_LOAD_PSP;
	a->ip_versions[MMHUB_HWIP][0] = IP_VERSION(4, 1, 0);
	dcn401_fixture_attach(a);
	dcn401_fixture_vbios(a);
	CHECK(block.version->funcs->early_init(&block) == 0);
	CHECK(block.version->funcs->sw_init(&block) == 0);
	CHECK(block.version->funcs->hw_init(&block) == 0);
}

/* ---- the fixture's vblank: flip interrupts and frame counters ---- */
static volatile int vblank_run = 1, vblank_hold;
static unsigned long vblanks;

static void raise_irq(unsigned int src_id)
{
	struct amdgpu_irq_src *src = adev->irq.client[SOC15_IH_CLIENTID_DCE].sources ?
		adev->irq.client[SOC15_IH_CLIENTID_DCE].sources[src_id] : NULL;
	struct amdgpu_iv_entry entry = { .client_id = SOC15_IH_CLIENTID_DCE, .src_id = src_id };

	if (src && src->funcs && src->funcs->process)
		src->funcs->process(adev, src, &entry);
}

static void *vblank_main(void *arg)
{
	(void)arg;
	while (vblank_run) {
		usleep(16000);
		if (vblank_hold)
			continue;
		for (int i = 0; i < 4; i++) {
			uint32_t reg = dcn401_dce_base[regOTG0_OTG_STATUS_FRAME_COUNT_BASE_IDX] +
				       regOTG0_OTG_STATUS_FRAME_COUNT + i * (regOTG1_OTG_H_TOTAL - regOTG0_OTG_H_TOTAL);

			WREG32(reg, RREG32(reg) + 1);
		}
		for (int i = 0; i < 4; i++)
			raise_irq(DCN_1_0__SRCID__HUBP0_FLIP_INTERRUPT + i);
		__atomic_add_fetch(&vblanks, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

/* ---- a client surface in three DMA segments ---- */
struct fake_surface {
	uint8_t *run[3];
	uint64_t run_bytes[3], size;
	struct rt_surface_segment segment[3];
	int releases;
};

static uint32_t *pixel(struct fake_surface *s, uint32_t x, uint32_t y)
{
	uint64_t offset = (uint64_t)y * PITCH + x * 4;

	for (int i = 0; i < 3; i++) {
		if (offset < s->run_bytes[i])
			return (uint32_t *)(s->run[i] + offset);
		offset -= s->run_bytes[i];
	}
	return NULL;
}

static void fake_init(struct fake_surface *s)
{
	uint64_t pages = ((uint64_t)PITCH * H + PAGE - 1) / PAGE;
	const uint64_t split[3] = { pages / 3, pages / 3, pages - 2 * (pages / 3) };

	memset(s, 0, sizeof(*s));
	s->size = pages * PAGE;
	for (int i = 0; i < 3; i++) {
		s->run_bytes[i] = split[i] * PAGE;
		s->run[i] = aligned_alloc(PAGE, s->run_bytes[i]);
		CHECK(s->run[i]);
		s->segment[i] = (struct rt_surface_segment){ (uint64_t)(uintptr_t)s->run[i], s->run_bytes[i] };
	}
}

static void fake_fill(struct fake_surface *s, uint32_t value, const struct rt_surface_rect *r)
{
	struct rt_surface_rect all = { 0, 0, W, H };

	if (!r)
		r = &all;
	for (uint32_t y = r->y; y < r->y + r->height; y++)
		for (uint32_t x = r->x; x < r->x + r->width; x++)
			*pixel(s, x, y) = value ^ (x << 12) ^ y;
}

static void provider_release(void *context)
{
	((struct fake_surface *)context)->releases++;
}

/* The buffer HUBP0 scans out, as the CPU sees it. */
static uint32_t screen_pixel(uint32_t x, uint32_t y)
{
	uint32_t lo = RREG32(dcn401_dce_base[regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX] +
			     regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS);
	uint32_t hi = RREG32(dcn401_dce_base[regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX] +
			     regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH);
	uint8_t *fb = cs_fixture_vram_host(((uint64_t)hi << 32) | lo);

	CHECK(fb);
	return *(uint32_t *)(fb + (size_t)y * PITCH + x * 4);
}

static struct rt_display_present_stats wait_flipped(struct pci_dev *pdev, uint64_t flipped)
{
	struct rt_display_present_stats st;

	for (int i = 0; i < 400; i++) {
		usleep(5000);
		CHECK(rt_display_stats(pdev, &st) == 0);
		if (st.frames_flipped >= flipped || st.error)
			break;
	}
	return st;
}

int main(void)
{
	struct fake_surface surf;
	struct rt_surface_provider provider = { provider_release, &surf };
	struct rt_display_report report;
	struct rt_display_present_stats st;
	struct cs_fixture_stats before, after;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector, *hdmi = NULL;
	struct rt_surface *surface;
	struct pci_dev *pdev;
	pthread_t vblank;
	uint32_t handle;
	int r;

	CHECK(fw_table_register_embedded() == 0);
	cs_fixture_sdma_instances = 2;
	cs_fixture_display = display_init;
	pdev = cs_fixture_init();
	adev = cs_fixture_adev();

	/* A sink on HDMI-A-1 (forced on with a synthetic EDID). */
	dcn401_fixture_build_edid();
	drm_connector_list_iter_begin(adev_to_drm(adev), &iter);
	drm_for_each_connector_iter(connector, &iter)
		if (!strcmp(connector->name, "HDMI-A-1"))
			hdmi = connector;
	drm_connector_list_iter_end(&iter);
	CHECK(hdmi);
	hdmi->force = DRM_FORCE_ON;
	CHECK(drm_edid_override_set(hdmi, dcn401_fixture_edid, sizeof(dcn401_fixture_edid)) == 0);
	CHECK(rt_display_probe(pdev, &report) == 0);

	CHECK(pthread_create(&vblank, NULL, vblank_main, NULL) == 0);
	r = rt_display_output(pdev, "HDMI-A-1", W, H, 60000, &report);
	printf("pipeline: output -> %d (commit %d)\n", r, report.commit_status);
	CHECK(r == 0 && report.showing && report.pattern == RT_DISPLAY_PATTERN_OUTPUT);

	fake_init(&surf);
	fake_fill(&surf, 0x00a00000u, NULL);
	CHECK(rt_surface_import(pdev, surf.segment, 3, surf.size, W, H, PITCH, &provider, &surface) == 0);
	handle = rt_surface_add(1, surface);
	CHECK(handle);

	/* Frame 1: the whole frame (every buffer starts stale), on both
	 * engines. PRESENT returns before anything ran. */
	{
		const struct rt_surface_rect all = { 0, 0, W, H };
		u64 t0 = ktime_get_ns(), dt;

		cs_fixture_stats(&before);
		r = rt_display_present(pdev, rt_surface_get_hold(1, handle), &all, 1, NULL, 0, ktime_get_ns(), &st);
		dt = ktime_get_ns() - t0;
		printf("pipeline: present 1 -> %d in %llu us (queued: received %llu, flipped %llu)\n", r,
		       (unsigned long long)dt / 1000, (unsigned long long)st.frames_received,
		       (unsigned long long)st.frames_flipped);
		CHECK(r == 0 && st.version == 3 && st.engines == 2 && st.frames_received == 1);
		st = wait_flipped(pdev, 1);
		cs_fixture_stats(&after);
		printf("pipeline: frame 1 flipped: %llu bytes in %llu job(s), copy %llu us, latency %llu us, "
		       "%lu SDMA copy packets\n", (unsigned long long)st.bytes, (unsigned long long)st.copy_jobs,
		       (unsigned long long)st.last_copy_gpu_ns / 1000,
		       (unsigned long long)st.last_latency_ns / 1000, after.copies - before.copies);
		CHECK(!st.error && st.frames_flipped == 1 && st.full_frames == 1);
		CHECK(st.bytes == (uint64_t)PITCH * H && st.copy_jobs == 2);
		CHECK(st.last_latency_ns > 0 && after.faults == 0 && after.dart_faults == 0);
		CHECK(screen_pixel(0, 0) == (0x00a00000u ^ 0) && screen_pixel(W - 1, H - 1) ==
		      (0x00a00000u ^ ((W - 1) << 12) ^ (H - 1)));
	}

	/* Frames 2-4, one at a time (each flips before the next): the worker
	 * alternates between the two buffers not on screen. Only a frame's
	 * own damage comes from the surface (over PCIe); what its buffer
	 * missed comes from the buffer drawn last, in VRAM. Frame 2 lands in
	 * the buffer the output started on, stale: all of it from frame 1's
	 * buffer; frame 3 in frame 1's buffer: frame 2's damage from frame
	 * 2's; frame 4 in frame 2's: frame 3's damage from frame 3's. */
	{
		const struct rt_surface_rect box[3] = { { 100, 100, 200, 50 }, { 400, 300, 64, 64 },
							{ 0, 1000, W, 80 } };
		const uint64_t vram[3] = { (uint64_t)PITCH * H, 200 * 4 * 50, 64 * 4 * 64 };

		for (int f = 0; f < 3; f++) {
			const uint64_t vram_before = st.vram_bytes;
			struct cs_fixture_stats packets_before, packets_after;

			cs_fixture_stats(&packets_before);
			fake_fill(&surf, 0x00b00000u + (uint32_t)f, &box[f]);
			r = rt_display_present(pdev, rt_surface_get_hold(1, handle), &box[f], 1, NULL, 0, ktime_get_ns(), &st);
			CHECK(r == 0);
			st = wait_flipped(pdev, 2 + (uint64_t)f);
			printf("pipeline: frame %d flipped: %llu bytes from the surface (last %llu), %llu in VRAM, full frames %u\n",
			       2 + f, (unsigned long long)st.bytes, (unsigned long long)st.last_bytes,
			       (unsigned long long)(st.vram_bytes - vram_before), st.full_frames);
			CHECK(!st.error && st.frames_flipped == 2 + (uint64_t)f);
			CHECK(st.last_bytes == (uint64_t)box[f].width * 4 * box[f].height);
			CHECK(st.vram_bytes - vram_before == vram[f]);
			/* A rectangle is one packet (a sub-window copy, or one range
			 * for whole rows, in copy_max_bytes pieces), not one per
			 * row: what the buffer missed and the frame's own. */
			cs_fixture_stats(&packets_after);
			{
				const uint64_t max = adev->mman.buffer_funcs->copy_max_bytes;

				CHECK(packets_after.copies - packets_before.copies == (vram[f] + max - 1) / max + 1);
			}
		}
		CHECK(st.full_frames == 1);
		CHECK(screen_pixel(150, 120) == ((0x00b00000u) ^ (150u << 12) ^ 120u));
		CHECK(screen_pixel(410, 310) == ((0x00b00001u) ^ (410u << 12) ^ 310u));
		CHECK(screen_pixel(5, 1010) == ((0x00b00002u) ^ (5u << 12) ^ 1010u));
		CHECK(screen_pixel(1000, 500) == ((0x00a00000u) ^ (1000u << 12) ^ 500u));
	}

	/* No vblank: frames pile up in the mailbox, the newest wins, the
	 * damage of the replaced ones is kept. */
	{
		const struct rt_surface_rect a = { 10, 10, 10, 10 }, b = { 50, 50, 10, 10 };
		uint64_t flipped;

		st = wait_flipped(pdev, 4);
		flipped = st.frames_flipped;
		vblank_hold = 1;
		usleep(40000);
		for (int f = 0; f < 4; f++) {
			fake_fill(&surf, 0x00c00000u + (uint32_t)f, (f & 1) ? &b : &a);
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), (f & 1) ? &b : &a, 1, NULL, 0,
						 ktime_get_ns(), &st) == 0);
			usleep(5000);
		}
		CHECK(st.frames_replaced >= 1);
		vblank_hold = 0;
		st = wait_flipped(pdev, flipped + 2);
		usleep(100000);
		CHECK(rt_display_stats(pdev, &st) == 0);
		printf("pipeline: 4 frames while no flip: received %llu, replaced %llu, flipped %llu\n",
		       (unsigned long long)st.frames_received, (unsigned long long)st.frames_replaced,
		       (unsigned long long)st.frames_flipped);
		CHECK(!st.error && st.frames_flipped < flipped + 4);
		CHECK(screen_pixel(15, 15) == ((0x00c00002u) ^ (15u << 12) ^ 15u));
		CHECK(screen_pixel(55, 55) == ((0x00c00003u) ^ (55u << 12) ^ 55u));
	}

	/* A window redrawing in place (the same rectangle every frame, with
	 * a menu-bar item inside it): each flip copies the region once, not
	 * once for the frame and again for each buffer that missed it. */
	{
		const struct rt_surface_rect win[2] = { { 300, 200, 400, 300 }, { 310, 210, 20, 20 } };
		const uint64_t once = 400ull * 4 * 300;

		for (int f = 0; f < 6; f++) {
			uint64_t flipped;

			CHECK(rt_display_stats(pdev, &st) == 0);
			flipped = st.frames_flipped;
			fake_fill(&surf, 0x00d00000u + (uint32_t)f, &win[0]);
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), win, 2, NULL, 0,
						 ktime_get_ns(), &st) == 0);
			st = wait_flipped(pdev, flipped + 1);
			if (f >= 2)
				CHECK(st.last_bytes == once);
		}
		printf("pipeline: a window redrawn in place copies %llu bytes per flip\n",
		       (unsigned long long)st.last_bytes);
		CHECK(screen_pixel(500, 350) == ((0x00d00005u) ^ (500u << 12) ^ 350u));
	}

	/* A window moving across the screen (the agent's move workload), with
	 * frames replaced in the mailbox while flips are held off: each frame
	 * redraws the background where the window was and the window where it
	 * is, damage being the two rectangles. Afterwards the screen must equal
	 * the surface everywhere: no trail of an old position. */
	{
		const uint32_t ww = 240, wh = 160;
		struct rt_surface_rect was = { 0, 0, 0, 0 };
		int32_t x = 5, y = 7, dx = 37, dy = 23;
		uint64_t flipped;
		unsigned long bad = 0;

		CHECK(rt_display_stats(pdev, &st) == 0);
		flipped = st.frames_flipped;
		fake_fill(&surf, 0x00f00000u, NULL);
		{
			const struct rt_surface_rect all = { 0, 0, W, H };

			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), &all, 1, NULL, 0,
						 ktime_get_ns(), &st) == 0);
			st = wait_flipped(pdev, flipped + 1);
		}
		for (int f = 0; f < 120; f++) {
			struct rt_surface_rect now = { (uint32_t)x, (uint32_t)y, ww, wh }, damage[2];
			uint32_t n = 0;

			if (was.width) {
				fake_fill(&surf, 0x00f00000u, &was);	/* background back */
				damage[n++] = was;
			}
			fake_fill(&surf, 0x00f10000u + (uint32_t)f, &now);
			damage[n++] = now;
			/* Every few frames, flips stall: frames pile up in the mailbox. */
			vblank_hold = (f % 10) >= 6;
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), damage, n, NULL, 0,
						 ktime_get_ns(), &st) == 0);
			usleep((f % 7) == 0 ? 1000 : 9000);
			was = now;
			x += dx; y += dy;
			if (x < 0 || x + (int32_t)ww > (int32_t)W) { dx = -dx; x += 2 * dx; }
			if (y < 0 || y + (int32_t)wh > (int32_t)H) { dy = -dy; y += 2 * dy; }
		}
		vblank_hold = 0;
		CHECK(rt_display_stats(pdev, &st) == 0);
		st = wait_flipped(pdev, st.frames_received - st.frames_replaced);
		usleep(100000);
		for (uint32_t py = 0; py < H; py++)
			for (uint32_t px = 0; px < W; px++)
				if (screen_pixel(px, py) != *pixel(&surf, px, py) && bad++ < 5)
					printf("pipeline: moving window: pixel %u,%u is %08x, the surface has %08x\n",
					       px, py, screen_pixel(px, py), *pixel(&surf, px, py));
		CHECK(rt_display_stats(pdev, &st) == 0);
		printf("pipeline: a moving window over 120 frames (%llu replaced in the mailbox): %lu pixels differ\n",
		       (unsigned long long)st.frames_replaced, bad);
		CHECK(bad == 0);
	}

	/* Scrolling: a document under a fixed header (whole width), then a
	 * scroll view inside a window (part of the width). Each frame moves
	 * the rows that scrolled (VRAM to VRAM from the buffer drawn last)
	 * and damages only the rows it exposed, plus now and then a header
	 * row (a clock). Some frames pile up in the mailbox: their moves
	 * become damage. Afterwards the screen equals the surface. */
	{
		static const struct {
			uint32_t x, y, w, h;
		} view[2] = { { 0, 64, W, H - 64 }, { 300, 200, 1000, 700 } };

		for (int v = 0; v < 2; v++) {
			const uint32_t vx = view[v].x, vy = view[v].y, vw = view[v].w, vh = view[v].h;
			int32_t off = 1000;
			uint64_t flipped, moved0, dropped0, vram0, bytes0, frames0;
			unsigned long bad = 0;

			CHECK(rt_display_stats(pdev, &st) == 0);
			flipped = st.frames_flipped;
			fake_fill(&surf, 0x00e00000u, NULL);	/* the header, the rest of the desktop */
			for (uint32_t y = 0; y < vh; y++)
				for (uint32_t x = 0; x < vw; x++)
					*pixel(&surf, vx + x, vy + y) = ((uint32_t)(off + (int32_t)y) * 0x9e3779b1u) ^ (x << 12);
			{
				const struct rt_surface_rect all = { 0, 0, W, H };

				CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), &all, 1, NULL, 0,
							 ktime_get_ns(), &st) == 0);
				st = wait_flipped(pdev, flipped + 1);
			}
			moved0 = st.moved_rows;
			dropped0 = st.moves_dropped;
			vram0 = st.vram_bytes;
			bytes0 = st.bytes;
			frames0 = st.frames_received;
			for (int f = 0; f < 60; f++) {
				static const int32_t steps[] = { 3, 17, 40, 1, 120, -9, -64, 25, 7, -1 };
				const int32_t step = steps[f % 10];
				const uint32_t k = (uint32_t)(step < 0 ? -step : step);
				struct rt_display_move mv = { vx, step > 0 ? vy : vy + k, vw, vh - k,
							      step > 0 ? vy + k : vy, 0 };
				struct rt_surface_rect damage[2] = {
					{ vx, step > 0 ? vy + vh - k : vy, vw, k },
				};
				uint32_t n = 1;

				off += step;
				for (uint32_t y = 0; y < vh; y++)
					for (uint32_t x = 0; x < vw; x++)
						*pixel(&surf, vx + x, vy + y) =
							((uint32_t)(off + (int32_t)y) * 0x9e3779b1u) ^ (x << 12);
				if (v == 0 && (f % 4) == 0) {
					const struct rt_surface_rect clock = { W - 200, 20, 120, 24 };

					fake_fill(&surf, 0x00c10000u + (uint32_t)f, &clock);
					damage[n++] = clock;
				}
				vblank_hold = (f % 12) >= 9;
				CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), damage, n, &mv, 1,
							 ktime_get_ns(), &st) == 0);
				usleep((f % 5) == 0 ? 1000 : 9000);
			}
			vblank_hold = 0;
			CHECK(rt_display_stats(pdev, &st) == 0);
			st = wait_flipped(pdev, st.frames_received - st.frames_replaced);
			usleep(100000);
			for (uint32_t py = 0; py < H; py++)
				for (uint32_t px = 0; px < W; px++)
					if (screen_pixel(px, py) != *pixel(&surf, px, py) && bad++ < 5)
						printf("pipeline: scroll %d: pixel %u,%u is %08x, the surface has %08x\n",
						       v, px, py, screen_pixel(px, py), *pixel(&surf, px, py));
			CHECK(rt_display_stats(pdev, &st) == 0 && !st.error);
			printf("pipeline: scrolling %s over 60 frames: %llu rows moved in VRAM, %llu moves copied from the surface (mailbox), %.1f KB from the surface and %.1f KB in VRAM per frame; %lu pixels differ\n",
			       v ? "a view inside a window" : "under a fixed header",
			       (unsigned long long)(st.moved_rows - moved0), (unsigned long long)(st.moves_dropped - dropped0),
			       (double)(st.bytes - bytes0) / 1e3 / (double)(st.frames_received - frames0),
			       (double)(st.vram_bytes - vram0) / 1e3 / (double)(st.frames_received - frames0), bad);
			CHECK(bad == 0);
			CHECK(st.moved_rows > moved0 && st.moves_dropped > dropped0 && st.vram_bytes > vram0);
		}
	}

	/* A move outside the frame is refused, the surface hold released and
	 * the output unharmed. */
	{
		const struct rt_display_move out[] = {
			{ 0, H - 10, W, 11, 0, 0 },	/* rows past the bottom */
			{ 0, 0, W, 10, H - 5, 0 },	/* source rows past the bottom */
			{ W - 10, 0, 11, 10, 0, 0 },	/* columns past the right */
			{ 0, 0, 0, 10, 0, 0 },		/* empty */
			{ 0, 0, 10, 10, 0, 1 },		/* reserved set */
		};
		/* Edges: the whole frame up a row (source to the last row), then
		 * down a row (destination to the last row), only the exposed
		 * row damaged each time. */
		const struct rt_display_move up = { 0, 0, W, H - 1, 1, 0 }, down = { 0, 1, W, H - 1, 0, 0 };
		const struct rt_surface_rect last = { 0, H - 1, W, 1 }, first = { 0, 0, W, 1 };
		unsigned long bad = 0;
		uint64_t flipped;

		for (unsigned int i = 0; i < sizeof(out) / sizeof(out[0]); i++)
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), NULL, 0, &out[i], 1,
						 ktime_get_ns(), &st) == -EINVAL);
		CHECK(rt_display_stats(pdev, &st) == 0 && !st.error);
		for (int pass = 0; pass < 2; pass++) {
			flipped = st.frames_flipped;
			if (pass == 0) {
				for (uint32_t y = 0; y + 1 < H; y++)
					for (uint32_t x = 0; x < W; x++)
						*pixel(&surf, x, y) = *pixel(&surf, x, y + 1);
				fake_fill(&surf, 0x00d20000u, &last);
			} else {
				for (uint32_t y = H - 1; y > 0; y--)
					for (uint32_t x = 0; x < W; x++)
						*pixel(&surf, x, y) = *pixel(&surf, x, y - 1);
				fake_fill(&surf, 0x00d30000u, &first);
			}
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), pass ? &first : &last, 1,
						 pass ? &down : &up, 1, ktime_get_ns(), &st) == 0);
			st = wait_flipped(pdev, flipped + 1);
		}
		usleep(50000);
		for (uint32_t py = 0; py < H; py++)
			for (uint32_t px = 0; px < W; px++)
				if (screen_pixel(px, py) != *pixel(&surf, px, py) && bad++ < 5)
					printf("pipeline: edge moves: pixel %u,%u is %08x, the surface has %08x\n",
					       px, py, screen_pixel(px, py), *pixel(&surf, px, py));
		printf("pipeline: moves past the frame refused; the whole frame up and down a row: %lu pixels differ\n", bad);
		CHECK(bad == 0);
		CHECK(rt_display_stats(pdev, &st) == 0 && !st.error);
	}

	/* A frame of another size is refused; nothing breaks. */
	CHECK(rt_display_stats(pdev, &st) == 0 && !st.error);
	printf("pipeline: average copy %llu us, latency %llu us (max %llu), worker submit %llu us per frame\n",
	       (unsigned long long)(st.copy_gpu_ns / st.frames_flipped / 1000),
	       (unsigned long long)(st.latency_ns / st.frames_flipped / 1000),
	       (unsigned long long)st.latency_max_ns / 1000,
	       (unsigned long long)(st.copy_submit_ns / st.frames_flipped / 1000));

	/* Off: the worker stops, the screen is restored, no frame is lost
	 * in flight. */
	CHECK(rt_display_off(pdev, &report) == 0 && !report.showing && !rt_display_showing());
	CHECK(rt_display_present(pdev, rt_surface_get_hold(1, handle), NULL, 0, NULL, 0, 0, &st) == -ENOENT);
	CHECK(rt_surface_remove(1, handle) == 0);
	for (int i = 0; i < 5000 && !surf.releases; i++)
		usleep(1000);
	CHECK(surf.releases == 1);
	/* The GPU powered off while the display is live: from the removal on,
	 * nothing reaches the hardware. No register write, no SDMA copy, no
	 * store into the CPU's VRAM aperture; presents fail, the output stops
	 * and nothing is committed. */
	{
		struct fake_surface live;
		struct rt_surface *lsurf;
		struct cs_fixture_stats at_removal, later;
		struct linuxu_aperture_stats ap0, ap1;
		static uint8_t aperture[1 << 16];
		const struct rt_surface_rect win = { 64, 64, 256, 128 };
		unsigned long regs;
		uint32_t lhandle;
		uint64_t flipped;

		r = rt_display_output(pdev, "HDMI-A-1", W, H, 60000, &report);
		CHECK(r == 0 && report.showing);
		fake_init(&live);
		fake_fill(&live, 0x00e00000u, NULL);
		CHECK(rt_surface_import(pdev, live.segment, 3, live.size, W, H, PITCH, &provider, &lsurf) == 0);
		lhandle = rt_surface_add(1, lsurf);
		CHECK(lhandle);
		linuxu_aperture_set((uintptr_t)aperture, sizeof(aperture));
		regs = __atomic_load_n(&dcn401_reg_writes, __ATOMIC_RELAXED);
		cs_fixture_stats(&at_removal);
		for (int f = 0; f < 4; f++) {
			CHECK(rt_display_stats(pdev, &st) == 0);
			flipped = st.frames_flipped;
			CHECK(rt_display_present(pdev, rt_surface_get_hold(1, lhandle), &win, 1, NULL, 0,
						 ktime_get_ns(), &st) == 0);
			st = wait_flipped(pdev, flipped + 1);
		}
		/* A frame queued and the device leaves the bus. */
		CHECK(rt_display_present(pdev, rt_surface_get_hold(1, lhandle), &win, 1, NULL, 0,
					 ktime_get_ns(), &st) == 0);
		/* Live: flips write registers and the copies run on SDMA. */
		cs_fixture_stats(&later);
		CHECK(__atomic_load_n(&dcn401_reg_writes, __ATOMIC_RELAXED) > regs);
		CHECK(later.copies > at_removal.copies);
		CHECK(!rt_removal_begin(pdev));
		CHECK(linuxu_aperture_is_gone());
		usleep(20000);	/* a copy or commit already past its check finishes */
		regs = __atomic_load_n(&dcn401_reg_writes, __ATOMIC_RELAXED);
		cs_fixture_stats(&at_removal);
		linuxu_aperture_stats(&ap0);
		memset(aperture, 0x5a, sizeof(aperture));
		for (int f = 0; f < 8; f++) {
			/* DMUB commands and kmap'd VRAM writes go through these. */
			linuxu_device_memcpy(aperture + 64, &win, sizeof(win));
			linuxu_device_memset(aperture + 4096, 0, 256);
			r = rt_display_present(pdev, rt_surface_get_hold(1, lhandle), &win, 1, NULL, 0,
					       ktime_get_ns(), &st);
			CHECK(r == 0 || r == -ENODEV);
			usleep(17000);
		}
		{
			uint32_t word = 0;

			linuxu_device_memcpy(&word, aperture, sizeof(word));
			CHECK(word == UINT32_MAX);	/* a vanished device reads all ones */
		}
		CHECK(rt_display_off(pdev, &report) == 0);
		CHECK(rt_surface_remove(1, lhandle) == 0);
		usleep(50000);
		cs_fixture_stats(&later);
		linuxu_aperture_stats(&ap1);
		for (size_t i = 0; i < sizeof(aperture); i++)
			CHECK(aperture[i] == 0x5a);
		printf("pipeline: after removal: %lu register writes, %lu SDMA copies, %llu aperture "
		       "operations refused\n", __atomic_load_n(&dcn401_reg_writes, __ATOMIC_RELAXED) - regs,
		       later.copies - at_removal.copies, ap1.skipped - ap0.skipped);
		CHECK(__atomic_load_n(&dcn401_reg_writes, __ATOMIC_RELAXED) == regs);
		CHECK(later.copies == at_removal.copies && later.sdma_ibs == at_removal.sdma_ibs);
		CHECK(ap1.skipped - ap0.skipped >= 17);
		linuxu_aperture_set(0, 0);
		rt_removal_end();
	}

	vblank_run = 0;
	pthread_join(vblank, NULL);
	cs_fixture_stats(&after);
	CHECK(after.faults == 0 && after.dart_faults == 0);
	printf("PASS display pipeline: queued presents, SDMA copies on two engines into three buffers, "
	       "nonblocking flips on the flip interrupt, damage tracking, mailbox, stop (%lu vblanks)\n",
	       vblanks);
	cs_fixture_stop();
	return 0;
}
