/* A CAMetalLayer for the RADV scanout test (test_radv_scanout.c): no
 * window, the size a swapchain is created at. */
#import <AppKit/AppKit.h>
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

/* An NSView backed by @layer, as SDL's Metal view is. */
void *test_metal_view(void *layer)
{
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1920, 1080)];

	view.wantsLayer = YES;
	view.layer = (CAMetalLayer *)layer;
	return (void *)view;
}

/* An NSView with an ordinary layer. */
void *test_plain_view(void)
{
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];

	view.wantsLayer = YES;
	return (void *)view;
}

void test_view_release(void *view)
{
	[(NSView *)view release];
}
