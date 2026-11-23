/*
 * SweetMapple: Open Mapple's Metal driver (VMDevice, Gallium Panfrost) under the names kernel patch 0126 gives Metal:
 * class SweetMTLDevice in /System/Library/Extensions/SweetMTLDriver.bundle. The workflow copies this file into
 * mapple_gpu_mali's driver/metal/, so it is linked with the rest of their driver; the bundle's executable is
 * SweetMapple, next to metal2's SweetMTLDriver, and Info.plist decides which of the two Metal loads.
 *
 * As metal2's SweetMTLDriver.mm: only the processes named in /var/sweet/metal.allow get a device (+alloc returns nil
 * for the others, so nothing half-made is released), Core Animation is kept off the deep framebuffer, and Metal's
 * validation failures are logged instead of aborting.
 *
 * Logs: /var/tmp/sweetmapple.log (this file), /var/tmp/sweetvmg.<process>.<pid>.log (the driver's own messages,
 * TSMTL_LOG on), /var/tmp/sweetmdrm.log (sweet-mapple-drm.c).
 */
#import "VMPrivate.h"
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#import "../../vgpu/iokit_compat.h"

extern int gTSMtlLogOn;

static void sm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm_log(const char *fmt, ...)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "MAPPLE[%d %s]: ", getpid(), getprogname());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	int fd = open("/var/tmp/sweetmapple.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		fchmod(fd, 0666);   /* tests run as root, backboardd does not */
		write(fd, buf, (size_t)n);
		close(fd);
	}
}

/* Is this process to get a device? Decided once. Without the file: the tests only. */
static bool allowed(void)
{
	static int answer = -1;
	if (answer >= 0)
		return answer;
	const char *me = getprogname();
	FILE *f = fopen("/var/sweet/metal.allow", "r");
	bool yes = false;
	if (!f) {
		yes = !strcmp(me, "vmprobe") || !strcmp(me, "mtltest") || !strcmp(me, "metaltest");
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

/* Core Animation's framebuffer as plain BGRA8: the Mali cannot scan out BGRA10_XR (see tools/metal2/SweetMTLDriver.mm). */
static void ca_plain_framebuffer(void)
{
	setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	CFStringRef keys[] = { CFSTR("CAEnableDeepFramebuffer"), CFSTR("CAEnableFramebufferCompression"),
			       CFSTR("CAEnableIOSurfaceCompression") };
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFPreferencesSetAppValue(keys[i], kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetValue(keys[i], kCFBooleanFalse, kCFPreferencesAnyApplication, kCFPreferencesCurrentUser,
				      kCFPreferencesAnyHost);
	}
	CFPreferencesSynchronize(kCFPreferencesAnyApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
}

__attribute__((constructor)) static void sm_loaded(void)
{
	if (!allowed())
		return;
	/* the driver's messages: on, and into a file of this process (backboardd's stderr goes nowhere) */
	setenv("TSMTL_LOG", "1", 0);
	gTSMtlLogOn = 1;
	char path[256];
	snprintf(path, sizeof(path), "/var/tmp/sweetvmg.%s.%d.log", getprogname(), getpid());
	if (freopen(path, "a", stderr))
		setvbuf(stderr, NULL, _IOLBF, 0);
	sm_log("bundle loaded (allowed, uid %d)", getuid());
	ca_plain_framebuffer();
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);
}

@interface VMDevice (SweetMapple)
- (instancetype)initWithAcceleratorPort:(io_service_t)port;
@end

@interface SweetMTLDevice : VMDevice
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

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	sm_log("initWithAcceleratorPort: %#x", port);
	self = [super initWithAcceleratorPort:port];
	if (self)
		sm_log("device: %s", self.name.UTF8String);
	else
		sm_log("no device (the reason is in /var/tmp/sweetvmg.%s.%d.log)", getprogname(), getpid());
	return self;
}

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines. */
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	(void)path;
	if (error)
		*error = [NSError errorWithDomain:@"SweetMapple" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}

@end
