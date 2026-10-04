/* Client framebuffers on the display output, offline: the fixture output
 * (scanout_fixture.c) and a libdrm-mlg client (test_scanout.c) through the
 * loopback transport, the dext's Linux-file core with the display hooks.
 * After the client's checks, its death: what it left on screen goes. */
#include <stdio.h>
#include <stdlib.h>
extern int usleep(unsigned int usec);

#include "mlg_drm.h"
#include "lx_loopback.h"
#include "cs_fixture.h"
#include "test_scanout.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

int main(void)
{
	struct mlg_transport transport;
	struct pci_dev *pdev = scanout_fixture_start(0x00102030u);
	uint64_t received, flipped;
	int error, r;

	CHECK(lx_loopback_transport(pdev, &transport) == 0 && mlg_drm_set_transport(&transport) == 0);
	lx_loopback_set_bar_memory(cs_fixture_bar_memory);
	r = test_scanout();
	if (r)
		return r;

	/* The client left its framebuffer on the primary plane: its death
	 * takes it off before its files close, and the desktop is back. */
	CHECK(scanout_fx_pipes_showing(TEST_SCANOUT_LEFT_ON_SCREEN));
	lx_loopback_exit();
	for (int i = 0; i < 300 && scanout_fx_pipes_showing(TEST_SCANOUT_LEFT_ON_SCREEN); i++)
		usleep(10000);
	CHECK(!scanout_fx_pipes_showing(TEST_SCANOUT_LEFT_ON_SCREEN));
	scanout_fx_desktop_stats(&received, &flipped, &error);
	CHECK(!error);
	printf("scanout: a closed client's frame left the screen; the desktop is back\n");
	scanout_fixture_finish();
	printf("PASS scanout: primary node without master, KMS queries, framebuffers, client frames "
	       "on the primary and overlay planes with flip syncobjs, held desktop frames, detach, "
	       "admission, relight with a client open, client death\n");
	return 0;
}
