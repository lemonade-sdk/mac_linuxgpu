/* The CS fixture device inside any Vulkan program: a library that, loaded
 * with DYLD_INSERT_LIBRARIES, brings up the fixture device (unmodified
 * upstream DRM/amdgpu over a software GPU, linuxu/tests/cs_fixture.c) and
 * installs the loopback transport before the program starts, so RADV in
 * that program reaches the fixture instead of the DriverKit extension.
 *
 *   MLG_PRELOAD=libmlg_fixture.dylib scripts/llama-radv.sh test-backend-ops perf -b Vulkan0
 *
 * (scripts/llama-radv.sh sets DYLD_INSERT_LIBRARIES for the tool alone).
 *
 * Built by scripts/test-llama-offline.sh. The software GPU executes no
 * shaders: programs see devices, memory, pipelines and fences, not
 * results. */
#include <stdio.h>
#include <stdlib.h>

#include "mlg_drm.h"
#include "cs_fixture.h"
#include "lx_loopback.h"

__attribute__((constructor)) static void fixture_preload(void)
{
	struct mlg_transport transport;
	struct pci_dev *pdev = cs_fixture_init();

	cs_fixture_model_driver_streams(1);
	if (lx_loopback_transport(pdev, &transport) || mlg_drm_set_transport(&transport)) {
		fprintf(stderr, "mlg fixture: no loopback transport\n");
		abort();
	}
	lx_loopback_set_bar_memory(cs_fixture_bar_memory);
	fprintf(stderr, "mlg fixture: software GPU ready (render node through the loopback)\n");
}
