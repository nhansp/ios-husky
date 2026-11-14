/*
 * vk_smoke: panvk on the husky panthor transport, without a Vulkan loader. Creates an instance through the ICD entry
 * point, prints the device, then runs a compute shader that writes gid * 2 + 1 into 1024 uints of a host-visible
 * buffer and waits on a fence. Prints PASS/FAIL lines to the console.
 */
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "fill_spv.h"

#define COUNT 1024

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

static int failures;

#define CHECK(cond, ...) do { \
	if (cond) { printf("vk_smoke: PASS " __VA_ARGS__); printf("\n"); } \
	else { failures++; printf("vk_smoke: FAIL " __VA_ARGS__); printf(" (line %d)\n", __LINE__); } \
} while (0)

#define VK_OK(call) do { \
	VkResult r_ = (call); \
	if (r_ != VK_SUCCESS) { printf("vk_smoke: FAIL %s -> %d (line %d)\n", #call, (int)r_, __LINE__); return finish(1); } \
} while (0)

static int
finish(int bad)
{
	if (bad) {
		failures++;
	}
	printf("vk_smoke: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	fflush(stdout);
	return failures ? 1 : 0;
}

#define GIP(name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)
#define GDP(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static int
find_memory_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) {
			return (int)i;
		}
	}
	return -1;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
	CHECK(vkCreateInstance != NULL, "vkCreateInstance resolved");
	if (vkCreateInstance == NULL) {
		return finish(1);
	}

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_smoke",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	VK_OK(vkCreateInstance(&ici, NULL, &instance));
	printf("vk_smoke: PASS vkCreateInstance\n");

	GIP(vkEnumeratePhysicalDevices);
	GIP(vkGetPhysicalDeviceProperties);
	GIP(vkGetPhysicalDeviceMemoryProperties);
	GIP(vkGetPhysicalDeviceQueueFamilyProperties);
	GIP(vkCreateDevice);
	GIP(vkGetDeviceProcAddr);

	uint32_t npd = 0;
	VK_OK(vkEnumeratePhysicalDevices(instance, &npd, NULL));
	CHECK(npd >= 1, "vkEnumeratePhysicalDevices count=%u", npd);
	if (npd < 1) {
		return finish(1);
	}
	npd = 1;
	VkPhysicalDevice pd;
	VkResult er = vkEnumeratePhysicalDevices(instance, &npd, &pd);
	if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
		printf("vk_smoke: FAIL vkEnumeratePhysicalDevices -> %d\n", (int)er);
		return finish(1);
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	const VkPhysicalDeviceLimits *l = &props.limits;
	printf("vk_smoke: device \"%s\" type=%d vendor=0x%x id=0x%x api=%u.%u.%u driver=0x%x\n", props.deviceName,
	    (int)props.deviceType, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
	    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
	printf("vk_smoke: limits maxImageDimension2D=%u maxComputeSharedMemorySize=%u maxComputeWorkGroupInvocations=%u "
	    "maxComputeWorkGroupSize=%ux%ux%u maxStorageBufferRange=%u maxBoundDescriptorSets=%u "
	    "maxPerStageDescriptorStorageBuffers=%u nonCoherentAtomSize=%llu minMemoryMapAlignment=%zu "
	    "timestampPeriod=%g\n", l->maxImageDimension2D, l->maxComputeSharedMemorySize,
	    l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
	    l->maxComputeWorkGroupSize[2], l->maxStorageBufferRange, l->maxBoundDescriptorSets,
	    l->maxPerStageDescriptorStorageBuffers, (unsigned long long)l->nonCoherentAtomSize, l->minMemoryMapAlignment,
	    l->timestampPeriod);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		printf("vk_smoke: memtype %u flags=0x%x heap=%u (%llu MiB)\n", i, mp.memoryTypes[i].propertyFlags,
		    mp.memoryTypes[i].heapIndex, (unsigned long long)(mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >> 20));
	}

	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
	VkQueueFamilyProperties qfp[8];
	if (nqf > 8) {
		nqf = 8;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfp);
	uint32_t qf = UINT32_MAX;
	for (uint32_t i = 0; i < nqf; i++) {
		printf("vk_smoke: queue family %u flags=0x%x count=%u\n", i, qfp[i].queueFlags, qfp[i].queueCount);
		if (qf == UINT32_MAX && (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
			qf = i;
		}
	}
	CHECK(qf != UINT32_MAX, "compute queue family %u", qf);
	if (qf == UINT32_MAX) {
		return finish(1);
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	VkDevice dev;
	VK_OK(vkCreateDevice(pd, &dci, NULL, &dev));
	printf("vk_smoke: PASS vkCreateDevice\n");

	GDP(vkGetDeviceQueue);
	GDP(vkCreateBuffer);
	GDP(vkGetBufferMemoryRequirements);
	GDP(vkAllocateMemory);
	GDP(vkBindBufferMemory);
	GDP(vkMapMemory);
	GDP(vkInvalidateMappedMemoryRanges);
	GDP(vkFlushMappedMemoryRanges);
	GDP(vkCreateShaderModule);
	GDP(vkCreateDescriptorSetLayout);
	GDP(vkCreatePipelineLayout);
	GDP(vkCreateComputePipelines);
	GDP(vkCreateDescriptorPool);
	GDP(vkAllocateDescriptorSets);
	GDP(vkUpdateDescriptorSets);
	GDP(vkCreateCommandPool);
	GDP(vkAllocateCommandBuffers);
	GDP(vkBeginCommandBuffer);
	GDP(vkEndCommandBuffer);
	GDP(vkCmdBindPipeline);
	GDP(vkCmdBindDescriptorSets);
	GDP(vkCmdDispatch);
	GDP(vkCmdPipelineBarrier);
	GDP(vkCreateFence);
	GDP(vkQueueSubmit);
	GDP(vkWaitForFences);

	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);

	const VkDeviceSize bytes = COUNT * sizeof(uint32_t);
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer buffer;
	VK_OK(vkCreateBuffer(dev, &bci, NULL, &buffer));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buffer, &mr);
	int mt = find_memory_type(&mp, mr.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	const int coherent = mt >= 0;
	if (mt < 0) {
		mt = find_memory_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	}
	CHECK(mt >= 0, "host-visible memory type %d (%s) size=%llu align=%llu", mt, coherent ? "coherent" : "non-coherent",
	    (unsigned long long)mr.size, (unsigned long long)mr.alignment);
	if (mt < 0) {
		return finish(1);
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = (uint32_t)mt };
	VkDeviceMemory mem;
	VK_OK(vkAllocateMemory(dev, &mai, NULL, &mem));
	VK_OK(vkBindBufferMemory(dev, buffer, mem, 0));
	uint32_t *data;
	VK_OK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&data));
	memset(data, 0xff, bytes);
	VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem, .offset = 0,
		.size = VK_WHOLE_SIZE };
	if (!coherent) {
		VK_OK(vkFlushMappedMemoryRanges(dev, 1, &range));
	}

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(fill_spv), .pCode = fill_spv };
	VkShaderModule sm;
	VK_OK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkDescriptorSetLayoutBinding binding = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding };
	VkDescriptorSetLayout dsl;
	VK_OK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &dsl };
	VkPipelineLayout pl;
	VK_OK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = sm, .pName = "main" }, .layout = pl };
	VkPipeline pipe;
	VK_OK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	printf("vk_smoke: PASS vkCreateComputePipelines\n");

	VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &dps };
	VkDescriptorPool dp;
	VK_OK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
		.descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	VK_OK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo dbi = { .buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
	VkCommandPool cp;
	VK_OK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	VK_OK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VK_OK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdDispatch(cb, COUNT / 64, 1, 1);
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
	    NULL);
	VK_OK(vkEndCommandBuffer(cb));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VK_OK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	VK_OK(vkQueueSubmit(queue, 1, &si, fence));
	printf("vk_smoke: PASS vkQueueSubmit\n");
	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000000000ull);
	CHECK(wr == VK_SUCCESS, "vkWaitForFences -> %d", (int)wr);
	if (wr != VK_SUCCESS) {
		return finish(1);
	}

	if (!coherent) {
		VK_OK(vkInvalidateMappedMemoryRanges(dev, 1, &range));
	}
	uint32_t bad = 0, first_bad = 0;
	for (uint32_t i = 0; i < COUNT; i++) {
		if (data[i] != i * 2 + 1) {
			if (bad++ == 0) {
				first_bad = i;
			}
		}
	}
	if (bad) {
		printf("vk_smoke: first mismatch data[%u]=0x%x expected 0x%x\n", first_bad, data[first_bad],
		    first_bad * 2 + 1);
	}
	CHECK(bad == 0, "compute result: %u of %u words wrong", bad, COUNT);
	return finish(0);
}
