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

/* Metal validation failures abort by default; in allowed processes they are logged (5 = NSLog), as in ios-flame. */
__attribute__((constructor)) static void sm2_loaded(void)
{
	if (!allowed())
		return;
	sm2_log("bundle loaded (allowed)");
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);
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
		/* plain rows: no Apple framebuffer compression, no 10-bit deep framebuffer */
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
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
	});
	return device;
}

@end
