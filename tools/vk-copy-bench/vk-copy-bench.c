/* vk-copy-bench: how fast the AMD GPU copies, through RADV, on each kind of
 * queue: the transfer queue (SDMA; RADV offers it with
 * RADV_EXPERIMENTAL=transfer_queue) and the compute queue (RADV copies
 * with a compute shader there). For each: host memory to VRAM (what the
 * display's frames cross PCIe as), VRAM to host, and VRAM to VRAM, timed
 * by GPU timestamps around the copies (wall time when a queue has none).
 *
 *   vk-copy-bench [--mib N] [--repeat N]
 *
 * Run with the RADV ICD: VK_DRIVER_FILES=.../radeon_icd.json. Every failure
 * is reported with the call that failed; nothing is measured in its place. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define FAIL(...) do { fprintf(stderr, "vk-copy-bench: " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) FAIL("%s failed: %d", #call, r_); } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mem_props;
static uint32_t g_families[8], g_nfamilies;	/* the queue families the buffers are shared by */

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want &&
		    !(mem_props.memoryTypes[i].propertyFlags & avoid))
			return (int)i;
	return -1;
}

struct buffer { VkBuffer buf; VkDeviceMemory mem; void *map; };

static struct buffer make_buffer(VkDeviceSize size, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid,
				 const char *what)
{
	struct buffer b = { 0 };
	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
				  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				  .sharingMode = VK_SHARING_MODE_CONCURRENT };
	bi.queueFamilyIndexCount = g_nfamilies;
	bi.pQueueFamilyIndices = g_families;
	if (g_nfamilies < 2)
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK(vkCreateBuffer(dev, &bi, NULL, &b.buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b.buf, &req);
	int type = memory_type(req.memoryTypeBits, want, avoid);
	if (type < 0)
		FAIL("no memory type for the %s buffer (flags 0x%x without 0x%x)", what, want, avoid);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
				    .memoryTypeIndex = (uint32_t)type };
	VK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
	VK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
	if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		VK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
	return b;
}

int main(int argc, char **argv)
{
	uint32_t mib = 64, repeat = 8;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mib") && i + 1 < argc) mib = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = (uint32_t)atoi(argv[++i]);
		else FAIL("usage: vk-copy-bench [--mib N] [--repeat N]");
	}
	if (!mib || !repeat)
		FAIL("--mib and --repeat must be positive");
	const VkDeviceSize size = (VkDeviceSize)mib << 20;

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-copy-bench",
				  .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
				     .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR };
	const char *ext[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
	ici.enabledExtensionCount = 1;
	ici.ppEnabledExtensionNames = ext;
	VkInstance inst;
	VK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	VK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties props;
	for (uint32_t i = 0; i < n; i++) {
		vkGetPhysicalDeviceProperties(pds[i], &props);
		if (props.vendorID == 0x1002) { pd = pds[i]; break; }
	}
	if (!pd)
		FAIL("no AMD GPU among %u Vulkan device(s) (is VK_DRIVER_FILES the RADV ICD?)", n);
	printf("device: %s\n", props.deviceName);
	vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);

	uint32_t nq = 8;
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	int transfer = -1, compute = -1;
	for (uint32_t i = 0; i < nq; i++) {
		VkQueueFlags f = qf[i].queueFlags;
		if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) && transfer < 0)
			transfer = (int)i;
		if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && compute < 0)
			compute = (int)i;
	}
	if (compute < 0)
		FAIL("no compute-only queue family");
	if (transfer < 0)
		printf("no transfer-only queue family (SDMA): run with RADV_EXPERIMENTAL=transfer_queue; measuring compute only\n");

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci[2];
	uint32_t nqci = 0;
	int fams[2] = { transfer, compute };
	for (int i = 0; i < 2; i++) {
		if (fams[i] < 0) continue;
		qci[nqci] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
						       .queueFamilyIndex = (uint32_t)fams[i], .queueCount = 1,
						       .pQueuePriorities = &prio };
		g_families[g_nfamilies++] = (uint32_t)fams[i];
		nqci++;
	}
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = nqci,
				   .pQueueCreateInfos = qci };
	VK(vkCreateDevice(pd, &dci, NULL, &dev));

	struct buffer host_a = make_buffer(size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "host");
	struct buffer host_b = make_buffer(size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "second host");
	struct buffer vram_a = make_buffer(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "VRAM");
	struct buffer vram_b = make_buffer(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "second VRAM");
	memset(host_a.map, 0x5a, size);
	(void)host_b;

	for (int q = 0; q < 2; q++) {
		if (fams[q] < 0) continue;
		const uint32_t family = (uint32_t)fams[q];
		const char *name = q == 0 ? "transfer (SDMA)" : "compute (shader)";
		VkQueue queue;
		vkGetDeviceQueue(dev, family, 0, &queue);
		VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
						.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
						.queueFamilyIndex = family };
		VkCommandPool pool;
		VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
		VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
						    .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
						    .commandBufferCount = 1 };
		VkCommandBuffer cb;
		VK(vkAllocateCommandBuffers(dev, &cai, &cb));
		const bool stamps = qf[family].timestampValidBits > 0;
		VkQueryPool qp = VK_NULL_HANDLE;
		if (stamps) {
			VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
						      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
			VK(vkCreateQueryPool(dev, &qpi, NULL, &qp));
		}
		VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkFence fence;
		VK(vkCreateFence(dev, &fci, NULL, &fence));
		const struct { const char *what; VkBuffer src, dst; } cases[] = {
			{ "host -> VRAM", host_a.buf, vram_a.buf },
			{ "VRAM -> host", vram_a.buf, host_b.buf },
			{ "VRAM -> VRAM", vram_a.buf, vram_b.buf },
		};
		for (unsigned c = 0; c < 3; c++) {
			double best = 0, total_gb = 0, total_s = 0;
			for (uint32_t rep = 0; rep < repeat + 1; rep++) {	/* the first warms up */
				VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
								.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
				VK(vkBeginCommandBuffer(cb, &bi));
				if (stamps) {
					vkCmdResetQueryPool(cb, qp, 0, 2);
					vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
				}
				VkBufferCopy region = { 0, 0, size };
				vkCmdCopyBuffer(cb, cases[c].src, cases[c].dst, 1, &region);
				if (stamps)
					vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
				VK(vkEndCommandBuffer(cb));
				VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
						    .pCommandBuffers = &cb };
				const uint64_t t0 = now_ns();
				VK(vkQueueSubmit(queue, 1, &si, fence));
				VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
				const uint64_t wall = now_ns() - t0;
				VK(vkResetFences(dev, 1, &fence));
				double seconds = (double)wall / 1e9;
				if (stamps) {
					uint64_t ts[2];
					VK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
								 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
					seconds = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e9;
				}
				if (!rep || seconds <= 0)
					continue;
				const double gbs = (double)size / seconds / 1e9;
				if (gbs > best) best = gbs;
				total_gb += (double)size / 1e9;
				total_s += seconds;
			}
			printf("%-17s %-13s %6.2f GB/s average, %6.2f GB/s best (%u x %u MiB, %s)\n", name, cases[c].what,
			       total_s > 0 ? total_gb / total_s : 0, best, repeat, mib, stamps ? "GPU timestamps" : "wall time");
		}
		vkDestroyFence(dev, fence, NULL);
		if (qp) vkDestroyQueryPool(dev, qp, NULL);
		vkDestroyCommandPool(dev, pool, NULL);
	}
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return 0;
}
