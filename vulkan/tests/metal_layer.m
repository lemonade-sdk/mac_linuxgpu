/* A CAMetalLayer for the RADV scanout test (test_radv_scanout.c): no
 * window, the size a swapchain is created at. */
#import <QuartzCore/CAMetalLayer.h>

void *test_metal_layer(unsigned width, unsigned height)
{
	CAMetalLayer *layer = [[CAMetalLayer alloc] init];

	layer.contentsScale = 1.0;
	layer.bounds = CGRectMake(0, 0, width, height);
	return (void *)layer;
}

void test_metal_layer_release(void *layer)
{
	[(CAMetalLayer *)layer release];
}
