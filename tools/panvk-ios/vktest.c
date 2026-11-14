/*
 * vktest: is PanVK on iOS good enough for metal2vulkan? (docs/METAL-PANVK.md, phase 1)
 *
 * Opens /usr/local/lib/libvulkan_panfrost.dylib the way the Metal bridge will (dlopen + its exported
 * vkGetInstanceProcAddr), prints what the device offers (our vulkaninfo), then runs four GPU checks:
 *   fill     vkCmdFillBuffer on host-visible memory, read back
 *   clear    vkCmdClearColorImage on a 64x64 RGBA8 image, copied to a buffer, read back
 *   copy     buffer -> buffer copy, read back
 *   timeline a timeline semaphore signalled by a submit, waited on by the CPU
 * metal2vulkan needs Vulkan >= 1.2 and timelineSemaphore. Last line: "vktest: RESULT ..." .
 * Log: /var/tmp/sweetvk.log (Mesa's own messages: /var/tmp/sweetvk-stderr.log). A 90 s alarm ends a hung run.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRIVER "/usr/local/lib/libvulkan_panfrost.dylib"

static FILE *out;

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
}

static void
on_alarm(int sig)
{
	(void)sig;
	say("vktest: RESULT timeout (90 s): the GPU or a wait hung");
	_exit(3);
}

static double
ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) / 1e6;
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static const char *
vkr(VkResult r)
{
	static char buf[24];
	switch (r) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "MEMORY_MAP_FAILED";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "INCOMPATIBLE_DRIVER";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
	default: snprintf(buf, sizeof(buf), "%d", (int)r); return buf;
	}
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { say("vktest: %s -> %s", #call, vkr(r_)); return 0; } } while (0)

static VkPhysicalDeviceMemoryProperties memprops;
static uint32_t qfamily;
static VkQueue queue;
static VkCommandPool pool;

static int
host_memory_type(uint32_t bits)
{
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags &
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			return (int)i;
	return -1;
}

static int
device_memory_type(uint32_t bits)
{
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			return (int)i;
	return host_memory_type(bits);
}

struct buf { VkBuffer b; VkDeviceMemory m; void *p; };

static int
make_buffer(struct buf *o, VkDeviceSize size, VkBufferUsageFlags usage)
{
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory);
	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	CHECK(vkCreateBuffer(dev, &bi, NULL, &o->b));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, o->b, &req);
	int type = host_memory_type(req.memoryTypeBits);
	if (type < 0) { say("vktest: no host-visible coherent memory type"); return 0; }
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &o->m));
	CHECK(vkBindBufferMemory(dev, o->b, o->m, 0));
	CHECK(vkMapMemory(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p));
	return 1;
}

/* Record with `record`, submit, wait on a fence. */
static int
run(const char *what, void (*record)(VkCommandBuffer, void *), void *ctx)
{
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit);
	DFN(vkCreateFence); DFN(vkWaitForFences); DFN(vkDestroyFence); DFN(vkFreeCommandBuffers);
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CHECK(vkBeginCommandBuffer(cb, &bi));
	record(cb, ctx);
	CHECK(vkEndCommandBuffer(cb));
	VkFence fence;
	VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	CHECK(vkCreateFence(dev, &fi, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
	say("vktest: %s: submitted and finished in %.1f ms", what, ms_since(&t0));
	vkDestroyFence(dev, fence, NULL);
	vkFreeCommandBuffers(dev, pool, 1, &cb);
	return 1;
}

static struct buf A, B;
static VkImage image;

static void
rec_fill(VkCommandBuffer cb, void *ctx)
{
	(void)ctx;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkCmdFillBuffer);
	vkCmdFillBuffer(cb, A.b, 0, 65536, 0xdeadbeef);
}

static void
rec_copy(VkCommandBuffer cb, void *ctx)
{
	(void)ctx;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkCmdCopyBuffer);
	VkBufferCopy r = { 0, 0, 65536 };
	vkCmdCopyBuffer(cb, A.b, B.b, 1, &r);
}

static void
rec_clear(VkCommandBuffer cb, void *ctx)
{
	(void)ctx;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkCmdPipelineBarrier); DFN(vkCmdClearColorImage); DFN(vkCmdCopyImageToBuffer);
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	VkClearColorValue red = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
	vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy c = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 64, 64, 1 } };
	vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, B.b, 1, &c);
}

static int
count_bad(const uint32_t *p, unsigned n, uint32_t want, uint32_t *first)
{
	int bad = 0;
	for (unsigned i = 0; i < n; i++)
		if (p[i] != want) {
			if (!bad) *first = p[i];
			bad++;
		}
	return bad;
}

static int
gpu_tests(int timeline, int *passed)
{
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	DFN(vkGetDeviceQueue); DFN(vkCreateCommandPool); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements);
	DFN(vkAllocateMemory); DFN(vkBindImageMemory); DFN(vkCreateSemaphore); DFN(vkQueueSubmit);
	DFN(vkWaitSemaphores); DFN(vkGetSemaphoreCounterValue);
	uint32_t first = 0;
	int bad;

	vkGetDeviceQueue(dev, qfamily, 0, &queue);
	VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfamily };
	CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));
	if (!make_buffer(&A, 65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
	    !make_buffer(&B, 65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
		return 0;
	say("vktest: buffers mapped at %p and %p", A.p, B.p);

	/* fill */
	memset(A.p, 0, 65536);
	if (!run("fill", rec_fill, NULL)) return 0;
	bad = count_bad(A.p, 16384, 0xdeadbeef, &first);
	say("vktest: fill: %d of 16384 words wrong%s", bad, bad ? "" : " (PASS)");
	if (bad) say("vktest: fill: first wrong word %#x", first);
	*passed += !bad;

	/* copy */
	memset(B.p, 0, 65536);
	if (!run("copy", rec_copy, NULL)) return 0;
	bad = count_bad(B.p, 16384, 0xdeadbeef, &first);
	say("vktest: copy: %d of 16384 words wrong%s", bad, bad ? "" : " (PASS)");
	if (bad) say("vktest: copy: first wrong word %#x", first);
	*passed += !bad;

	/* clear */
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	CHECK(vkCreateImage(dev, &ii, NULL, &image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, image, &req);
	VkDeviceMemory im;
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = (uint32_t)device_memory_type(req.memoryTypeBits) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, &im));
	CHECK(vkBindImageMemory(dev, image, im, 0));
	memset(B.p, 0, 65536);
	if (!run("clear", rec_clear, NULL)) return 0;
	bad = count_bad(B.p, 4096, 0xff0000ffu, &first);
	say("vktest: clear: %d of 4096 texels wrong%s", bad, bad ? "" : " (PASS)");
	if (bad) say("vktest: clear: first wrong texel %#x", first);
	*passed += !bad;

	/* timeline */
	if (!timeline) {
		say("vktest: timeline: not supported, skipped");
		return 1;
	}
	VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
	VkSemaphore sem;
	CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem));
	uint64_t value = 1;
	VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem };
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
		.pSemaphores = &sem, .pValues = &value };
	VkResult wr = vkWaitSemaphores(dev, &wi, 2000000000ull);
	uint64_t now = 0;
	vkGetSemaphoreCounterValue(dev, sem, &now);
	say("vktest: timeline: wait -> %s, counter %llu%s", vkr(wr), (unsigned long long)now,
	    wr == VK_SUCCESS && now == 1 ? " (PASS)" : "");
	*passed += wr == VK_SUCCESS && now == 1;
	return 1;
}

int
main(void)
{
	out = fopen("/var/tmp/sweetvk.log", "a");
	if (!out) out = stdout;
	freopen("/var/tmp/sweetvk-stderr.log", "a", stderr);
	setvbuf(stderr, NULL, _IONBF, 0);
	signal(SIGALRM, on_alarm);
	alarm(90);
	/* PanVK refuses v6/v7 without it ("not well-tested"); PANVK_DEBUG=startup logs how it finds the device. */
	setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
	setenv("PANVK_DEBUG", "startup", 0);
	setenv("MESA_DEBUG", "1", 0);
	time_t wall = time(NULL);
	say("vktest: start %s", ctime(&wall));

	void *lib = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);
	if (!lib) { say("vktest: RESULT dlopen failed: %s", dlerror()); return 1; }
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	say("vktest: vkGetInstanceProcAddr %s", gipa ? "exported" : "MISSING");
	if (!gipa) { say("vktest: RESULT the driver does not export vkGetInstanceProcAddr"); return 1; }

	PFN_vkEnumerateInstanceVersion eiv = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	uint32_t iv = VK_API_VERSION_1_0;
	if (eiv) eiv(&iv);
	say("vktest: instance version %u.%u.%u", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv));

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktest",
		.apiVersion = iv >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : iv };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	VkResult r = vkCreateInstance(&ici, NULL, &inst);
	say("vktest: vkCreateInstance -> %s (%.1f ms)", vkr(r), ms_since(&t0));
	if (r != VK_SUCCESS) { say("vktest: RESULT no instance"); return 1; }

	IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties2); IFN(vkGetPhysicalDeviceFeatures2);
	IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkCreateDevice);
	uint32_t n = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	r = vkEnumeratePhysicalDevices(inst, &n, NULL);
	say("vktest: vkEnumeratePhysicalDevices -> %s, %u device(s) (%.1f ms)", vkr(r), n, ms_since(&t0));
	if (r != VK_SUCCESS || n == 0) { say("vktest: RESULT no device (see sweetvk-stderr.log and sweetdrm.log)"); return 1; }
	VkPhysicalDevice pd;
	n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);

	VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
	vkGetPhysicalDeviceProperties2(pd, &p2);
	VkPhysicalDeviceProperties *p = &p2.properties;
	say("vktest: device \"%s\", Vulkan %u.%u.%u, vendor %#x device %#x, driver %s %s", p->deviceName,
	    VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion),
	    p->vendorID, p->deviceID, drv.driverName, drv.driverInfo);
	say("vktest: limits: image2D %u, push constants %u, storage buffer range %u, uniform buffer range %u, "
	    "bound sets %u, compute workgroup %u x %u x %u (invocations %u), minStorageBufferOffsetAlignment %llu",
	    p->limits.maxImageDimension2D, p->limits.maxPushConstantsSize, p->limits.maxStorageBufferRange,
	    p->limits.maxUniformBufferRange, p->limits.maxBoundDescriptorSets, p->limits.maxComputeWorkGroupSize[0],
	    p->limits.maxComputeWorkGroupSize[1], p->limits.maxComputeWorkGroupSize[2],
	    p->limits.maxComputeWorkGroupInvocations, (unsigned long long)p->limits.minStorageBufferOffsetAlignment);

	VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
	VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
	if (p->apiVersion < VK_API_VERSION_1_3) f12.pNext = NULL;
	if (p->apiVersion < VK_API_VERSION_1_2) f11.pNext = NULL;
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	VkPhysicalDeviceFeatures *f = &f2.features;
	say("vktest: 1.0: shaderInt64 %u shaderInt16 %u shaderFloat64 %u geometryShader %u tessellation %u "
	    "independentBlend %u dualSrcBlend %u depthClamp %u depthBiasClamp %u fillModeNonSolid %u wideLines %u "
	    "samplerAnisotropy %u textureCompressionETC2 %u textureCompressionASTC_LDR %u textureCompressionBC %u "
	    "fragmentStoresAndAtomics %u vertexPipelineStoresAndAtomics %u imageCubeArray %u multiDrawIndirect %u "
	    "drawIndirectFirstInstance %u robustBufferAccess %u shaderClipDistance %u sampleRateShading %u",
	    f->shaderInt64, f->shaderInt16, f->shaderFloat64, f->geometryShader, f->tessellationShader,
	    f->independentBlend, f->dualSrcBlend, f->depthClamp, f->depthBiasClamp, f->fillModeNonSolid, f->wideLines,
	    f->samplerAnisotropy, f->textureCompressionETC2, f->textureCompressionASTC_LDR, f->textureCompressionBC,
	    f->fragmentStoresAndAtomics, f->vertexPipelineStoresAndAtomics, f->imageCubeArray, f->multiDrawIndirect,
	    f->drawIndirectFirstInstance, f->robustBufferAccess, f->shaderClipDistance, f->sampleRateShading);
	say("vktest: 1.1: storageBuffer16BitAccess %u variablePointers %u multiview %u shaderDrawParameters %u samplerYcbcrConversion %u",
	    f11.storageBuffer16BitAccess, f11.variablePointers, f11.multiview, f11.shaderDrawParameters, f11.samplerYcbcrConversion);
	say("vktest: 1.2: timelineSemaphore %u bufferDeviceAddress %u descriptorIndexing %u shaderInt8 %u "
	    "shaderFloat16 %u storageBuffer8BitAccess %u scalarBlockLayout %u imagelessFramebuffer %u "
	    "uniformBufferStandardLayout %u hostQueryReset %u vulkanMemoryModel %u samplerMirrorClampToEdge %u "
	    "drawIndirectCount %u separateDepthStencilLayouts %u runtimeDescriptorArray %u descriptorBindingPartiallyBound %u",
	    f12.timelineSemaphore, f12.bufferDeviceAddress, f12.descriptorIndexing, f12.shaderInt8, f12.shaderFloat16,
	    f12.storageBuffer8BitAccess, f12.scalarBlockLayout, f12.imagelessFramebuffer, f12.uniformBufferStandardLayout,
	    f12.hostQueryReset, f12.vulkanMemoryModel, f12.samplerMirrorClampToEdge, f12.drawIndirectCount,
	    f12.separateDepthStencilLayouts, f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound);
	say("vktest: 1.3: dynamicRendering %u synchronization2 %u maintenance4 %u shaderDemoteToHelperInvocation %u "
	    "subgroupSizeControl %u inlineUniformBlock %u",
	    f13.dynamicRendering, f13.synchronization2, f13.maintenance4, f13.shaderDemoteToHelperInvocation,
	    f13.subgroupSizeControl, f13.inlineUniformBlock);

	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
	VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
	say("vktest: %u device extensions:", ne);
	char line[400] = "";
	for (uint32_t i = 0; i < ne; i++) {
		if (strlen(line) + strlen(ext[i].extensionName) + 2 > sizeof(line) - 1) {
			say("vktest:   %s", line);
			line[0] = 0;
		}
		strcat(line, ext[i].extensionName);
		strcat(line, " ");
	}
	if (line[0]) say("vktest:   %s", line);
	int host_mem = 0;
	for (uint32_t i = 0; i < ne; i++)
		if (!strcmp(ext[i].extensionName, "VK_EXT_external_memory_host")) host_mem = 1;
	say("vktest: VK_EXT_external_memory_host (IOSurface import for the Metal bridge): %s", host_mem ? "yes" : "no");

	vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
	for (uint32_t i = 0; i < memprops.memoryHeapCount; i++)
		say("vktest: heap %u: %llu MiB flags %#x", i, (unsigned long long)(memprops.memoryHeaps[i].size >> 20),
		    memprops.memoryHeaps[i].flags);
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
		say("vktest: memory type %u: heap %u flags %#x", i, memprops.memoryTypes[i].heapIndex,
		    memprops.memoryTypes[i].propertyFlags);
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties qf[8];
	if (nq > 8) nq = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	qfamily = 0;
	for (uint32_t i = 0; i < nq; i++)
		say("vktest: queue family %u: flags %#x count %u timestampValidBits %u", i, qf[i].queueFlags,
		    qf[i].queueCount, qf[i].timestampValidBits);

	int timeline = p->apiVersion >= VK_API_VERSION_1_2 && f12.timelineSemaphore;
	VkPhysicalDeviceVulkan12Features on12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = timeline ? VK_TRUE : VK_FALSE };
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfamily,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = timeline ? &on12 : NULL,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	clock_gettime(CLOCK_MONOTONIC, &t0);
	r = vkCreateDevice(pd, &dci, NULL, &dev);
	say("vktest: vkCreateDevice -> %s (%.1f ms)", vkr(r), ms_since(&t0));
	if (r != VK_SUCCESS) { say("vktest: RESULT no logical device"); return 1; }

	int passed = 0, total = timeline ? 4 : 3;
	int ran = gpu_tests(timeline, &passed);
	int ready = p->apiVersion >= VK_API_VERSION_1_2 && f12.timelineSemaphore;
	say("vktest: RESULT %d of %d GPU checks passed%s; Vulkan %u.%u, timelineSemaphore %u: %s for metal2vulkan",
	    passed, total, ran ? "" : " (stopped early)", VK_API_VERSION_MAJOR(p->apiVersion),
	    VK_API_VERSION_MINOR(p->apiVersion), f12.timelineSemaphore,
	    ready && passed == total ? "ready" : "NOT ready");
	fclose(out);
	_exit(0);       /* no teardown: this run is about what works, not about destruction */
}
