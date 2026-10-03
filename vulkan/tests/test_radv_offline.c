/* RADV offline: Mesa's Vulkan driver (libvulkan_radeon.dylib, built from
 * third_party/mesa) on the software GPU of the CS fixture device
 * (linuxu/tests/cs_fixture.c), through libdrm-mlg and libmlg_drm's loopback
 * transport into the unmodified upstream DRM/amdgpu.
 *
 * The ICD is loaded directly, as the Vulkan loader would load it
 * (vk_icdNegotiateLoaderICDInterfaceVersion, vk_icdGetInstanceProcAddr).
 *
 * Covers: instance, physical device enumeration (libdrm's device list),
 * properties and memory types from AMDGPU_INFO, device creation, buffers
 * and host-visible memory (GEM create, VA, mmap), command buffers on the
 * compute queue: an empty submit and a fence, vkCmdFillBuffer and
 * vkCmdUpdateBuffer (CP DMA and WRITE_DATA, which the software GPU runs,
 * checked through the mapping), a compute pipeline compiled by ACO and a
 * dispatch (submitted and fenced; the software GPU does not run shaders, so
 * its result is not checked), timeline semaphores, and teardown. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "mlg_drm.h"
#include "cs_fixture.h"
#include "lx_loopback.h"
#include "fill.spv.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); exit(1); } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #call, r_); exit(1); } } while (0)

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *version);

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkDevice device;

#define IFN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
			    VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

int main(int argc, char **argv)
{
	const char *icd_path = argc > 1 ? argv[1] : getenv("RADV_ICD");
	struct mlg_transport transport;
	struct cs_fixture_stats before, after;
	struct pci_dev *pdev;
	uint32_t version = 5;
	void *icd;

	CHECK(icd_path);
	pdev = cs_fixture_init();
	cs_fixture_model_driver_streams(1);
	CHECK(!lx_loopback_transport(pdev, &transport));
	lx_loopback_set_bar_memory(cs_fixture_bar_memory);
	CHECK(!mlg_drm_set_transport(&transport));

	icd = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
	if (!icd) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	PFN_negotiate negotiate = (PFN_negotiate)dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
	CHECK(negotiate && gipa);
	VK(negotiate(&version));
	printf("ICD interface version %u\n", version);

	/* Instance and the GPU. */
	IFN(vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "mac_linuxgpu radv offline", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VK(vkCreateInstance(&ici, NULL, &instance));
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceProperties2);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkCreateDevice);
	IFN(vkDestroyInstance);
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
	CHECK(gdpa);

	uint32_t count = 0;
	VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
	CHECK(count == 1);
	VkPhysicalDevice phys;
	VK(vkEnumeratePhysicalDevices(instance, &count, &phys));
	VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT };
	VkPhysicalDeviceDriverProperties driver = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &pci };
	VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &driver };
	vkGetPhysicalDeviceProperties2(phys, &props);
	printf("device: %s (%04x:%04x), %s %s, Vulkan %u.%u\n", props.properties.deviceName,
	       props.properties.vendorID, props.properties.deviceID, driver.driverName,
	       driver.driverInfo, VK_API_VERSION_MAJOR(props.properties.apiVersion),
	       VK_API_VERSION_MINOR(props.properties.apiVersion));
	CHECK(props.properties.vendorID == 0x1002 && props.properties.deviceID == 0x7551);
	CHECK(!strcmp(props.properties.deviceName, "AMD Radeon AI Pro R9700 (RADV GFX1201)"));
	CHECK(pci.pciBus == 0xc3 && pci.pciDevice == 0 && pci.pciFunction == 0);
	CHECK(driver.driverID == VK_DRIVER_ID_MESA_RADV);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
		printf("heap %u: %llu MiB%s\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
		       mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "");

	uint32_t nqf = 0, qf = UINT32_MAX;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
	VkQueueFamilyProperties qfp[8];
	CHECK(nqf <= 8);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfp);
	for (uint32_t i = 0; i < nqf; ++i) {
		printf("queue family %u: flags 0x%x, %u queue(s)\n", i, qfp[i].queueFlags,
		       qfp[i].queueCount);
		if (qf == UINT32_MAX && (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
			qf = i;
	}
	CHECK(qf != UINT32_MAX);

	/* The device, on the compute queue. */
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &priority };
	VkPhysicalDeviceVulkan12Features f12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .timelineSemaphore = VK_TRUE };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VK(vkCreateDevice(phys, &dci, NULL, &device));
	printf("device created\n");

	DFN(vkGetDeviceQueue);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkUnmapMemory);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkResetCommandBuffer);
	DFN(vkCmdFillBuffer);
	DFN(vkCmdUpdateBuffer);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdBindDescriptorSets);
	DFN(vkCmdPushConstants);
	DFN(vkCmdDispatch);
	DFN(vkQueueSubmit);
	DFN(vkCreateFence);
	DFN(vkWaitForFences);
	DFN(vkResetFences);
	DFN(vkGetFenceStatus);
	DFN(vkCreateSemaphore);
	DFN(vkWaitSemaphores);
	DFN(vkGetSemaphoreCounterValue);
	DFN(vkCreateShaderModule);
	DFN(vkCreateDescriptorSetLayout);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateComputePipelines);
	DFN(vkCreateDescriptorPool);
	DFN(vkAllocateDescriptorSets);
	DFN(vkUpdateDescriptorSets);
	DFN(vkDestroyPipeline);
	DFN(vkDestroyPipelineLayout);
	DFN(vkDestroyDescriptorSetLayout);
	DFN(vkDestroyDescriptorPool);
	DFN(vkDestroyShaderModule);
	DFN(vkDestroySemaphore);
	DFN(vkDestroyFence);
	DFN(vkDestroyCommandPool);
	DFN(vkDestroyBuffer);
	DFN(vkFreeMemory);
	DFN(vkDeviceWaitIdle);
	DFN(vkDestroyDevice);

	VkQueue queue;
	vkGetDeviceQueue(device, qf, 0, &queue);

	/* A host-visible buffer. */
	const VkDeviceSize size = 64 << 10;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buffer;
	VK(vkCreateBuffer(device, &bci, NULL, &buffer));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(device, buffer, &mr);
	const uint32_t mt = memory_type(&mp, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CHECK(mt != UINT32_MAX);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size, .memoryTypeIndex = mt };
	VkDeviceMemory memory;
	VK(vkAllocateMemory(device, &mai, NULL, &memory));
	VK(vkBindBufferMemory(device, buffer, memory, 0));
	uint32_t *cpu;
	VK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&cpu));
	memset(cpu, 0, size);
	printf("buffer: %llu bytes in memory type %u, mapped\n", (unsigned long long)size, mt);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
	VkCommandPool pool;
	VK(vkCreateCommandPool(device, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(device, &cbai, &cmd));
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VK(vkCreateFence(device, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
		.pCommandBuffers = &cmd };

	/* 1. An empty command buffer and a fence. */
	VK(vkBeginCommandBuffer(cmd, &begin));
	VK(vkEndCommandBuffer(cmd));
	VK(vkQueueSubmit(queue, 1, &si, fence));
	VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
	VK(vkGetFenceStatus(device, fence));
	printf("empty submit: fence signaled\n");

	/* 2. Fill and update, checked through the mapping. */
	static const uint32_t words[16] = { 0x600d0000, 0x600d0001, 0x600d0002, 0x600d0003,
		0x600d0004, 0x600d0005, 0x600d0006, 0x600d0007, 0x600d0008, 0x600d0009,
		0x600d000a, 0x600d000b, 0x600d000c, 0x600d000d, 0x600d000e, 0x600d000f };
	cs_fixture_stats(&before);
	VK(vkResetFences(device, 1, &fence));
	VK(vkResetCommandBuffer(cmd, 0));
	VK(vkBeginCommandBuffer(cmd, &begin));
	vkCmdFillBuffer(cmd, buffer, 0, 1024, 0xdeadbeef);
	vkCmdUpdateBuffer(cmd, buffer, 2048, sizeof(words), words);
	VK(vkEndCommandBuffer(cmd));
	VK(vkQueueSubmit(queue, 1, &si, fence));
	VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
	cs_fixture_stats(&after);
	for (uint32_t i = 0; i < 256; ++i)
		CHECK(cpu[i] == 0xdeadbeef);
	CHECK(cpu[256] == 0);
	for (uint32_t i = 0; i < 16; ++i)
		CHECK(cpu[512 + i] == words[i]);
	printf("fill + update: data checked (%lu compute IBs, %lu WRITE_DATA, %lu DMA_DATA)\n",
	       after.compute_ibs - before.compute_ibs, after.write_data - before.write_data,
	       after.dma_data - before.dma_data);

	/* 3. A compute pipeline (ACO) and a dispatch. */
	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(fill_spv), .pCode = fill_spv };
	VkShaderModule module;
	VK(vkCreateShaderModule(device, &smci, NULL, &module));
	VkDescriptorSetLayoutBinding binding = { .binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
		.pBindings = &binding };
	VkDescriptorSetLayout dsl;
	VK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr };
	VkPipelineLayout layout;
	VK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
	VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
		.layout = layout };
	VkPipeline pipeline;
	VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline));
	printf("compute pipeline compiled\n");

	VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
	VkDescriptorPool dpool;
	VK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	VK(vkAllocateDescriptorSets(device, &dsai, &set));
	VkDescriptorBufferInfo dbi = { buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(device, 1, &wds, 0, NULL);

	VkSemaphoreTypeCreateInfo stci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &stci };
	VkSemaphore timeline;
	VK(vkCreateSemaphore(device, &sci, NULL, &timeline));

	cs_fixture_stats(&before);
	const uint32_t value = 1000;
	VK(vkResetFences(device, 1, &fence));
	VK(vkResetCommandBuffer(cmd, 0));
	VK(vkBeginCommandBuffer(cmd, &begin));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &value);
	vkCmdDispatch(cmd, (uint32_t)(size / 4 / 64), 1, 1);
	VK(vkEndCommandBuffer(cmd));
	const uint64_t signal_value = 1;
	VkTimelineSemaphoreSubmitInfo tssi = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal_value };
	VkSubmitInfo dsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tssi,
		.commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1,
		.pSignalSemaphores = &timeline };
	VK(vkQueueSubmit(queue, 1, &dsi, fence));
	VkSemaphoreWaitInfo swi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &signal_value };
	VK(vkWaitSemaphores(device, &swi, 5000000000ull));
	VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
	uint64_t reached = 0;
	VK(vkGetSemaphoreCounterValue(device, timeline, &reached));
	CHECK(reached == 1);
	cs_fixture_stats(&after);
	CHECK(after.dispatches > before.dispatches);
	printf("dispatch: submitted, %lu dispatch packet(s) reached the compute engine, "
	       "timeline semaphore and fence signaled (shaders do not run on the fixture)\n",
	       after.dispatches - before.dispatches);

	/* Teardown. */
	VK(vkDeviceWaitIdle(device));
	vkDestroySemaphore(device, timeline, NULL);
	vkDestroyDescriptorPool(device, dpool, NULL);
	vkDestroyPipeline(device, pipeline, NULL);
	vkDestroyPipelineLayout(device, layout, NULL);
	vkDestroyDescriptorSetLayout(device, dsl, NULL);
	vkDestroyShaderModule(device, module, NULL);
	vkDestroyFence(device, fence, NULL);
	vkDestroyCommandPool(device, pool, NULL);
	vkUnmapMemory(device, memory);
	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, memory, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(instance, NULL);

	cs_fixture_stats(&after);
	CHECK(after.faults == 0);
	printf("PASS radv offline: instance, enumeration, device, buffers and memory, empty "
	       "submit with fence, fill/update data checked, ACO compute pipeline, dispatch "
	       "submitted with timeline semaphore and fence, teardown\n");
	return 0;
}
