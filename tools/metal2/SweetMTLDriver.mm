/*
 * SweetMTLDriver.bundle: the Metal driver of the Moto G05's Mali-G52 (docs/METAL-PANVK.md, phase 3).
 *
 * Metal.framework loads it in every process that asks for a device (kernel patch 0126: SweetAccelerator names the bundle
 * SweetMTLDriver and the class SweetMTLDevice). -initWithAcceleratorPort: hands back the frontend's device
 * (NativeBridgeMetalFrontend.mm, NativeMetal via ios-flame, MIT), which runs the metal2vulkan runtime (bridge/, statically
 * linked) on PanVK (/usr/local/lib/libvulkan_panfrost.dylib) over the kernel's SweetMali service.
 *
 * Which processes get a device: the names in /var/sweet/metal.allow (one per line); without the file, metaltest and
 * mtltest. Every other process (backboardd, SpringBoard...) gets none: +alloc returns nil, so no object is made and
 * nothing is torn down (boot 2026-11-14: returning nil from init let ARC release a never-initialised _MTLDevice and
 * backboardd crashed in a loop). backboardd stays off until IOSurface import works (PanVK lacks
 * VK_EXT_external_memory_host).
 *
 * After ios-flame's phone/metal2/SweetMetal2.mm (MIT). Log: /var/tmp/sweetmtl2.log, lines "MTL2[pid name]: ...".
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

/* the iOS SDK has no IOKitLib.h */
extern "C" kern_return_t IORegistryEntryGetRegistryEntryID(mach_port_t entry, uint64_t *entryID);

#define DRIVER "/usr/local/lib/libvulkan_panfrost.dylib"

extern "C" void sm2_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "MTL2[%d %s]: %s\n", getpid(), getprogname(), s);
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;
	int fd = open("/var/tmp/sweetmtl2.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		/* the tests run as root, backboardd does not: whoever creates the file leaves it writable for the others */
		fchmod(fd, 0666);
		write(fd, buf, (size_t)n);
		close(fd);
	}
}

static void sm2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm2_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sm2_logs(buf);
}

/*
 * The Mali does not snoop the CPU caches: libSystem's sys_dcache_flush (a no-op where the GPU is coherent) must really clean
 * and invalidate here, from user space. This definition wins at link time for the bridge and the frontend (IOSurface paths).
 */
extern "C" void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Is this process to get a device? Decided once. */
static bool allowed(void)
{
	static int answer = -1;
	if (answer >= 0)
		return answer;
	const char *me = getprogname();
	FILE *f = fopen("/var/sweet/metal.allow", "r");
	bool yes = false;
	if (!f) {
		yes = !strcmp(me, "metaltest") || !strcmp(me, "mtltest");
	} else {
		char line[256];
		while (!yes && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n \t")] = 0;
			yes = line[0] && !strcmp(line, me);
		}
		fclose(f);
	}
	answer = yes;
	return yes;
}

/* Selectors the frontend lacks, each logged once (the frontend then raises the usual unrecognised-selector exception). */
static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm2_log("MISSING %s -%s", cls, sel);
}

/*
 * Core Animation's framebuffer: plain BGRA8 rows. With CAEnableDeepFramebuffer on, CA's framebuffer is BGRA10_XR ('b3a8'),
 * which only Apple GPUs scan out: CA logs "Unexpected framebuffer format" and draws nothing (ios-flame HANDOFF, stage 3).
 * Boot 2026-11-14 with backboardd on Metal: XR textures (formats 550-554) were made, 18 passes, 0 draws, 0 commits in
 * 520 s, black screen. Set for this process (the app domain) and for every app of this user (the global domain), before
 * CA asks: this runs when Metal loads the bundle, earlier than -initWithAcceleratorPort:. sweetsvc also writes them into
 * the global preferences of root and mobile, so they hold from the next boot on even if CA reads them first.
 */
static void ca_plain_framebuffer(void)
{
	setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	CFStringRef keys[] = { CFSTR("CAEnableDeepFramebuffer"), CFSTR("CAEnableFramebufferCompression"),
			       CFSTR("CAEnableIOSurfaceCompression") };
	for (CFStringRef key : keys) {
		CFPreferencesSetAppValue(key, kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetValue(key, kCFBooleanFalse, kCFPreferencesAnyApplication, kCFPreferencesCurrentUser,
				      kCFPreferencesAnyHost);
	}
	CFPreferencesSynchronize(kCFPreferencesAnyApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFPropertyListRef deep = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication);
	sm2_log("CAEnableDeepFramebuffer now %s (uid %d)", deep == kCFBooleanFalse ? "off" : deep ? "ON" : "unset", getuid());
	if (deep)
		CFRelease(deep);
}

/* Metal validation failures abort by default; in allowed processes they are logged (5 = NSLog), as in ios-flame. */
__attribute__((constructor)) static void sm2_loaded(void)
{
	if (!allowed())
		return;
	sm2_log("bundle loaded (allowed, uid %d)", getuid());
	ca_plain_framebuffer();
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);
}

/*
 * Device queries the runtime has no measured answer for: the frontend logs "no device answer" and returns 0 for them
 * (mtltest, 2026-11-14). The Mali-G52 r1 MC2's own values, so callers that size work by them get real numbers.
 */
static void answer_gaps(id device)
{
	static const struct { const char *sel; uint64_t value; } answers[] = {
		{ "gpuCoreCount", 2 },                          /* MC2: two shader cores */
		{ "maxComputeThreadgroupMemory", 16384 },       /* Metal's minimum; PanVK v7 shared memory is no smaller */
		{ "maxVertexAmplificationCount", 1 },           /* no vertex amplification */
		{ "isLargeMRTSupported", 0 },
		{ "supportsComputeCompressedTextureWrite", 0 },
		{ "supportsConditionalLoadStore", 0 },
	};
	Class cls = object_getClass(device);
	for (size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); i++) {
		SEL sel = sel_registerName(answers[i].sel);
		uint64_t value = answers[i].value;
		Method m = class_getInstanceMethod(cls, sel);
		const char *types = m ? method_getTypeEncoding(m) : "Q16@0:8";
		/* integers and BOOLs come back in x0 alike */
		class_replaceMethod(cls, sel, imp_implementationWithBlock(^uint64_t(id me) {
			(void)me;
			return value;
		}), types);
	}
	sm2_log("answers: gpuCoreCount 2, maxComputeThreadgroupMemory 16384, and 4 more");
}

/*
 * backboardd's work, every 10 s into /var/tmp/sweetmtl2-telemetry.log (in the outbox), for the first 10 minutes:
 * passes, draws, pipelines made and refused, commits and their times (native_bridge_metal_telemetry).
 */
static void start_telemetry(void)
{
	if (strncmp(getprogname(), "backboardd", 10) != 0)
		return;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		static char text[8192];
		for (int i = 0; i < 60; i++) {
			sleep(10);
			size_t n = native_bridge_metal_telemetry(text, sizeof(text) - 1);
			text[n < sizeof(text) ? n : sizeof(text) - 1] = 0;
			char status[400] = "";
			native_bridge_metal_status(status, sizeof(status));
			int fd = open("/var/tmp/sweetmtl2-telemetry.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (fd >= 0) {
				dprintf(fd, "MTL2[%d %s] +%d s: %s\n%s\n", getpid(), getprogname(), (i + 1) * 10, status, text);
				close(fd);
			}
		}
	});
}

@interface NBDeviceMethods : NSObject
@end

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines itself. */
@interface NBDeviceMethods (SweetMTLDriver)
@end
@implementation NBDeviceMethods (SweetMTLDriver)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	if (error)
		*error = [NSError errorWithDomain:@"SweetMTLDriver" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
@end

@interface _MTLDevice : NSObject
@end

@interface SweetMTLDevice : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTLDevice

+ (instancetype)alloc
{
	return [self allocWithZone:NULL];
}

+ (instancetype)allocWithZone:(struct _NSZone *)zone
{
	if (!allowed())
		return nil;
	return [super allocWithZone:zone];
}

- (id)initWithAcceleratorPort:(mach_port_t)port
{
	/* this object is dropped for the frontend's device: initialised, so its -[_MTLDevice dealloc] has queues to release */
	if (!(self = [super init]))
		return nil;
	static id device;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		/* PanVK on Bifrost v7 (tools/panvk-ios/vktest.c): accept the GPU, and claim Vulkan 1.2 (metal2vulkan needs it) */
		setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
		setenv("MESA_VK_VERSION_OVERRIDE", "1.2", 0);
		char cache[256];
		mkdir("/var/sweet", 0755);
		mkdir("/var/sweet/m2v-cache", 0777);
		chmod("/var/sweet/m2v-cache", 0777);
		snprintf(cache, sizeof(cache), "/var/sweet/m2v-cache/%s", getprogname());
		mkdir(cache, 0777);
		if (access(cache, W_OK) != 0) {
			char dir[256];
			if (confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir) - 8)) {
				snprintf(cache, sizeof(cache), "%sm2v", dir);
				mkdir(cache, 0700);
			}
		}
		/* PanVK's own disk cache of compiled shaders (Mesa built with shader-cache, 2026-11-24): next to
		 * metal2vulkan's, so a boot does not compile again what the one before did. /var/sweet/no-mesa-cache
		 * turns it off. The runtime's VkPipeline log feeds the telemetry's "V vkpipe" row (they are made at draws). */
		if (access("/var/sweet/no-mesa-cache", F_OK) == 0) {
			setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
		} else {
			char mesa[300];
			snprintf(mesa, sizeof(mesa), "%s/mesa", cache);
			mkdir(mesa, 0777);
			setenv("MESA_SHADER_CACHE_DIR", mesa, 0);
			setenv("MESA_SHADER_CACHE_MAX_SIZE", "256M", 0);
		}
		setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
		@try {
			device = native_bridge_metal_open(DRIVER, cache, registryID, missing);
		} @catch (NSException *e) {
			sm2_log("device: exception %s", e.reason.UTF8String);
			device = nil;
		}
		sm2_log("device %s in %.0f ms (cache %s)%s%s", device ? [[device name] UTF8String] : "refused",
			(CFAbsoluteTimeGetCurrent() - t0) * 1000, cache, device ? "" : ": ", device ? "" : nmb_last_error());
		if (device) {
			answer_gaps(device);
			start_telemetry();
		}
	});
	return device;
}

@end
