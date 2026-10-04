/* RADV's Metal surfaces on the GPU's own display engine, offline: Mesa's
 * Vulkan driver (libvulkan_radeon.dylib with patches/mesa/radv-macos-
 * display-wsi.patch) on the CS fixture's software GPU, through libdrm-mlg
 * and the loopback transport, presenting to the display output of the
 * fixture's DCN 4.0.1 display (linuxu/tests/scanout_fixture.c).
 *
 * The ICD is loaded directly, as the Vulkan loader would load it. The
 * CAMetalLayer has no window (metal_layer.m), so the frames go full screen
 * unless MLG_WSI_MODE says windowed.
 *
 * Covers: VK_EXT_metal_surface; no presentation support without
 * MLG_WSI_OUTPUT, and a refused swapchain for a connector the output does
 * not drive; surface capabilities, formats and FIFO; a swapchain whose
 * images are tiled framebuffers on the output's primary plane in place of
 * the desktop, acquired and presented frame after frame (each present's
 * vblank before the next); the desktop back when the swapchain goes; a
 * windowed swapchain on an overlay plane, centred. The software GPU runs
 * no shaders, so what the images contain is not checked: which
 * framebuffers the planes scan out is. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>

#include "mlg_drm.h"
#include "cs_fixture.h"
#include "lx_loopback.h"
#include "test_scanout.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); exit(1); } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #call, r_); exit(1); } } while (0)
#define EVENTUALLY(cond) ({ int ok_ = 0; \
	for (int i_ = 0; i_ < 300 && !(ok_ = !!(cond)); i_++) usleep(10000); ok_; })

#define DRM_FORMAT_MOD_LINEAR 0ull

void *test_metal_layer(unsigned width, unsigned height);
void test_metal_layer_release(void *layer);

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *version);

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkDevice device;

#define IFN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

static int primary_is_desktop(void)
{
	uint32_t w, h;
	uint64_t mod;

	return scanout_fx_primary_fb(&w, &h, &mod) && w == 1920 && h == 1080 &&
	       mod == DRM_FORMAT_MOD_LINEAR;
}

static int primary_is_tiled_swapchain(void)
{
	uint32_t w, h;
	uint64_t mod;

	return scanout_fx_primary_fb(&w, &h, &mod) && w == 1920 && h == 1080 &&
	       mod != DRM_FORMAT_MOD_LINEAR;
}

int main(int argc, char **argv)
{
	const char *icd_path = argc > 1 ? argv[1] : getenv("RADV_ICD");
	struct mlg_transport transport;
	struct pci_dev *pdev;
	uint32_t version = 5;
	void *icd, *layer;

	CHECK(icd_path);
	pdev = scanout_fixture_start(0x00102030u);
	cs_fixture_model_driver_streams(1);
	CHECK(!lx_loopback_transport(pdev, &transport));
	lx_loopback_set_bar_memory(cs_fixture_bar_memory);
	CHECK(!mlg_drm_set_transport(&transport));
	CHECK(EVENTUALLY(primary_is_desktop()));

	icd = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
	if (!icd) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	PFN_negotiate negotiate = (PFN_negotiate)dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
	CHECK(negotiate && gipa);
	VK(negotiate(&version));

	/* An instance with Metal surfaces. */
	IFN(vkEnumerateInstanceExtensionProperties);
	{
		VkExtensionProperties ext[64];
		uint32_t n = 64;
		int metal = 0;

		VK(vkEnumerateInstanceExtensionProperties(NULL, &n, ext));
		for (uint32_t i = 0; i < n; i++)
			metal |= !strcmp(ext[i].extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME);
		CHECK(metal);
	}
	IFN(vkCreateInstance);
	const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "mac_linuxgpu radv scanout", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = inst_ext };
	VK(vkCreateInstance(&ici, NULL, &instance));
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkCreateDevice);
	IFN(vkCreateMetalSurfaceEXT);
	IFN(vkDestroySurfaceKHR);
	IFN(vkGetPhysicalDeviceSurfaceSupportKHR);
	IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
	IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
	IFN(vkGetPhysicalDeviceSurfacePresentModesKHR);
	IFN(vkDestroyInstance);
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
	CHECK(gdpa && vkCreateMetalSurfaceEXT);

	uint32_t count = 1;
	VkPhysicalDevice phys;
	VK(vkEnumeratePhysicalDevices(instance, &count, &phys));
	uint32_t nqf = 0, qf = UINT32_MAX;
	VkQueueFamilyProperties qfp[8];
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
	CHECK(nqf <= 8);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfp);
	for (uint32_t i = 0; i < nqf && qf == UINT32_MAX; ++i)
		if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			qf = i;
	CHECK(qf != UINT32_MAX);

	layer = test_metal_layer(1920, 1080);
	VkMetalSurfaceCreateInfoEXT msci = { .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
		.pLayer = layer };
	VkSurfaceKHR surface;
	VK(vkCreateMetalSurfaceEXT(instance, &msci, NULL, &surface));

	/* Presentation needs MLG_WSI_OUTPUT. */
	VkBool32 supported = VK_TRUE;
	unsetenv("MLG_WSI_OUTPUT");
	VK(vkGetPhysicalDeviceSurfaceSupportKHR(phys, qf, surface, &supported));
	CHECK(!supported);
	setenv("MLG_WSI_OUTPUT", "HDMI-A-1", 1);
	unsetenv("MLG_WSI_MODE");
	VK(vkGetPhysicalDeviceSurfaceSupportKHR(phys, qf, surface, &supported));
	CHECK(supported);

	VkSurfaceCapabilitiesKHR caps;
	VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
	CHECK(caps.currentExtent.width == 1920 && caps.currentExtent.height == 1080);
	CHECK(caps.minImageCount == 3 && caps.supportedCompositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
	VkSurfaceFormatKHR formats[8];
	uint32_t nformats = 8;
	VK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nformats, formats));
	CHECK(nformats == 2 && formats[0].format == VK_FORMAT_B8G8R8A8_UNORM);
	VkPresentModeKHR modes[4];
	uint32_t nmodes = 4;
	VK(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nmodes, modes));
	CHECK(nmodes == 1 && modes[0] == VK_PRESENT_MODE_FIFO_KHR);

	/* The device, with swapchains. */
	const float priority = 1.0f;
	const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext };
	VK(vkCreateDevice(phys, &dci, NULL, &device));
	DFN(vkGetDeviceQueue);
	DFN(vkCreateSwapchainKHR);
	DFN(vkDestroySwapchainKHR);
	DFN(vkGetSwapchainImagesKHR);
	DFN(vkAcquireNextImageKHR);
	DFN(vkQueuePresentKHR);
	DFN(vkCreateSemaphore);
	DFN(vkDestroySemaphore);
	DFN(vkCreateCommandPool);
	DFN(vkDestroyCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdPipelineBarrier);
	DFN(vkQueueSubmit);
	DFN(vkDeviceWaitIdle);
	DFN(vkDestroyDevice);
	VkQueue queue;
	vkGetDeviceQueue(device, qf, 0, &queue);

	VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surface, .minImageCount = 3, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = { 1920, 1080 },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
	VkSwapchainKHR swapchain;

	/* A connector the output does not drive. */
	setenv("MLG_WSI_OUTPUT", "DP-1", 1);
	CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain) == VK_ERROR_INITIALIZATION_FAILED);
	CHECK(primary_is_desktop());
	setenv("MLG_WSI_OUTPUT", "HDMI-A-1", 1);

	VK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
	uint32_t nimages = 0;
	VK(vkGetSwapchainImagesKHR(device, swapchain, &nimages, NULL));
	CHECK(nimages >= 3 && nimages <= 8);
	VkImage images[8];
	VK(vkGetSwapchainImagesKHR(device, swapchain, &nimages, images));
	printf("radv scanout: swapchain of %u images\n", nimages);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
	VkCommandPool pool;
	VK(vkCreateCommandPool(device, &cpci, NULL, &pool));
	VkCommandBuffer cmd[8];
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = nimages };
	VK(vkAllocateCommandBuffers(device, &cbai, cmd));
	/* Each image only goes to PRESENT_SRC (the fixture runs no shaders). */
	for (uint32_t i = 0; i < nimages; i++) {
		VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = 0, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = images[i],
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

		VK(vkBeginCommandBuffer(cmd[i], &begin));
		vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		VK(vkEndCommandBuffer(cmd[i]));
	}
	VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkSemaphore acquired[8] = { 0 }, rendered[8] = { 0 };
	for (uint32_t i = 0; i < nimages; i++) {
		VK(vkCreateSemaphore(device, &semci, NULL, &acquired[i]));
		VK(vkCreateSemaphore(device, &semci, NULL, &rendered[i]));
	}

	/* Frames: acquire, "render", present; every image comes round. */
	uint32_t seen = 0;
	for (uint32_t f = 0; f < 3 * nimages; f++) {
		uint32_t index;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSemaphore a = acquired[f % nimages], r = rendered[f % nimages];

		VK(vkAcquireNextImageKHR(device, swapchain, 5000000000ull, a, VK_NULL_HANDLE, &index));
		CHECK(index < nimages);
		seen |= 1u << index;
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
			.pWaitSemaphores = &a, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
			.pCommandBuffers = &cmd[index], .signalSemaphoreCount = 1, .pSignalSemaphores = &r };
		VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &r, .swapchainCount = 1,
			.pSwapchains = &swapchain, .pImageIndices = &index };
		VK(vkQueuePresentKHR(queue, &pi));
	}
	printf("radv scanout: %u frames presented, images used 0x%x\n", 3 * nimages, seen);
	CHECK(seen == (1u << nimages) - 1);
	CHECK(EVENTUALLY(primary_is_tiled_swapchain()));
	{
		uint32_t w, h;
		uint64_t mod;

		CHECK(scanout_fx_primary_fb(&w, &h, &mod));
		printf("radv scanout: the primary plane scans out a %ux%u swapchain image, modifier "
		       "0x%llx\n", w, h, (unsigned long long)mod);
	}
	VK(vkDeviceWaitIdle(device));
	vkDestroySwapchainKHR(device, swapchain, NULL);
	/* The last swapchain gave the screen back. */
	CHECK(EVENTUALLY(primary_is_desktop()));
	printf("radv scanout: the desktop is back on the primary plane\n");

	/* A windowed swapchain with no window: an overlay plane, centred. */
	setenv("MLG_WSI_MODE", "windowed", 1);
	sci.imageExtent = (VkExtent2D){ 640, 360 };
	VK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
	VK(vkGetSwapchainImagesKHR(device, swapchain, &nimages, images));
	for (uint32_t i = 0; i < nimages; i++) {
		VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = images[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

		VK(vkBeginCommandBuffer(cmd[i], &begin));
		vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		VK(vkEndCommandBuffer(cmd[i]));
	}
	for (uint32_t f = 0; f < 4; f++) {
		uint32_t index;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSemaphore a = acquired[f % nimages], r = rendered[f % nimages];

		VK(vkAcquireNextImageKHR(device, swapchain, 5000000000ull, a, VK_NULL_HANDLE, &index));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
			.pWaitSemaphores = &a, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
			.pCommandBuffers = &cmd[index], .signalSemaphoreCount = 1, .pSignalSemaphores = &r };
		VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &r, .swapchainCount = 1,
			.pSwapchains = &swapchain, .pImageIndices = &index };
		VK(vkQueuePresentKHR(queue, &pi));
	}
	{
		int32_t x, y;
		uint32_t w, h;

		CHECK(EVENTUALLY(scanout_fx_overlay(&x, &y, &w, &h) && w == 640));
		printf("radv scanout: windowed: an overlay plane at %d,%d %ux%u over the desktop\n", x, y,
		       w, h);
		CHECK(x == (1920 - 640) / 2 && y == (1080 - 360) / 2 && h == 360);
		CHECK(primary_is_desktop());
	}
	VK(vkDeviceWaitIdle(device));
	vkDestroySwapchainKHR(device, swapchain, NULL);
	{
		int32_t x, y;
		uint32_t w, h;

		CHECK(EVENTUALLY(!scanout_fx_overlay(&x, &y, &w, &h)));
	}

	for (uint32_t i = 0; i < 8 && acquired[i]; i++) {
		vkDestroySemaphore(device, acquired[i], NULL);
		vkDestroySemaphore(device, rendered[i], NULL);
	}
	vkDestroyCommandPool(device, pool, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroySurfaceKHR(instance, surface, NULL);
	vkDestroyInstance(instance, NULL);
	test_metal_layer_release(layer);
	lx_loopback_exit();
	scanout_fixture_finish();
	printf("PASS radv scanout: VK_EXT_metal_surface on the display engine, MLG_WSI_OUTPUT, FIFO "
	       "frames on the primary plane, the desktop given back, a windowed overlay\n");
	return 0;
}
