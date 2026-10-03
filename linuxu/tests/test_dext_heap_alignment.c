#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

struct __attribute__((aligned(16))) aligned_resource {
	uint64_t values[2];
};

/* DRM managed resources have this alignment on arm64. Sanitizers must
 * reject the former 8-byte malloc result before it reaches the driver. */
int main(void)
{
	struct aligned_resource *resource = malloc(sizeof(*resource));
	assert(resource);
	resource->values[0] = 1;
	resource->values[1] = 2;
	assert(resource->values[0] + resource->values[1] == 3);
	free(resource);
	return 0;
}
