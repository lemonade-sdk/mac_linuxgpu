/* libdrm-mlg end to end (test_libdrm_mlg.c): the fixture device and the
 * loopback transport, then the client library's checks. */
#include <stdio.h>
#include <stdlib.h>

#include "mlg_drm.h"
#include "cs_fixture.h"
#include "lx_loopback.h"

int test_libdrm_mlg(void);

int main(void)
{
	struct mlg_transport transport;
	struct pci_dev *pdev = cs_fixture_init();
	int r;

	if (lx_loopback_transport(pdev, &transport) || mlg_drm_set_transport(&transport)) {
		fprintf(stderr, "test-libdrm-mlg: no loopback transport\n");
		return 1;
	}
	lx_loopback_set_bar_memory(cs_fixture_bar_memory);
	r = test_libdrm_mlg();
	lx_loopback_exit();
	cs_fixture_stop();
	return r;
}
