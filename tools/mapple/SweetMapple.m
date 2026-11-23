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
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#import "../../vgpu/iokit_compat.h"

extern int gTSMtlLogOn;
void vgpu_counters(uint64_t *calls, uint64_t *jobs);   /* sweet-mapple-drm.c (calls may be NULL) */

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

/*
 * Why a process dies (boot 2026-11-23: backboardd on Mapple made its device, imported the framebuffer, and was gone
 * 5-10 s later, 100 times over, with nothing on stderr): fatal signals with their address and the stack, uncaught
 * Objective-C exceptions, exit(), and a heartbeat with the GPU's job count. No line for a death means SIGKILL
 * (watchdog or jetsam): then the copied crash report says which.
 */
static void sm_write(int fd, const char *s)
{
	write(fd, s, strlen(s));
}

static void sm_signal(int sig, siginfo_t *info, void *context)
{
	char line[512];
	int fd = open("/var/tmp/sweetmapple.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	uint64_t pc = 0, lr = 0;
#if defined(__arm64__) && defined(arm_thread_state64_get_pc)
	ucontext_t *uc = context;
	if (uc && uc->uc_mcontext) {
		pc = (uint64_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uint64_t)arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
	}
#else
	(void)context;
#endif
	if (fd >= 0) {
		snprintf(line, sizeof(line), "MAPPLE[%d %s]: SIGNAL %d code %d addr %p pc %#llx lr %#llx\n", getpid(), getprogname(),
		    sig, info ? info->si_code : 0, info ? info->si_addr : NULL, (unsigned long long)pc, (unsigned long long)lr);
		sm_write(fd, line);
		void *frames[48];
		int n = backtrace(frames, 48);
		uint64_t extra[2] = { pc, lr };
		for (int i = -2; i < n; i++) {
			void *a = i < 0 ? (void *)(uintptr_t)extra[i + 2] : frames[i];
			Dl_info di;
			if (a == NULL)
				continue;
			if (dladdr(a, &di) && di.dli_fname) {
				const char *file = strrchr(di.dli_fname, '/');
				snprintf(line, sizeof(line), "  %s %p %s+%#lx (%s)\n", i == -2 ? "pc" : i == -1 ? "lr" : "  ", a,
				    di.dli_sname ? di.dli_sname : "?", (unsigned long)((uintptr_t)a - (uintptr_t)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)),
				    file ? file + 1 : di.dli_fname);
			} else {
				snprintf(line, sizeof(line), "  %s %p\n", i == -2 ? "pc" : i == -1 ? "lr" : "  ", a);
			}
			sm_write(fd, line);
		}
		close(fd);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void sm_exception(NSException *e)
{
	sm_log("EXCEPTION %s: %s", e.name.UTF8String, e.reason.UTF8String);
	for (NSString *s in e.callStackSymbols)
		sm_log("  %s", s.UTF8String);
}

static void sm_exit(void)
{
	sm_log("exit() called");
}

/* The newest crash report whose name starts with prefix, its first 48 KiB into /var/tmp/sweetcrash.<prefix>.log. */
static void sm_copy_report(const char *prefix)
{
	const char *dirs[] = { "/var/mobile/Library/Logs/CrashReporter", "/private/var/mobile/Library/Logs/CrashReporter/Retired" };
	char best[512] = "";
	time_t newest = 0;
	for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *e;
		if (!dir)
			continue;
		while ((e = readdir(dir)) != NULL) {
			char path[512];
			struct stat st;
			if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
			if (stat(path, &st) == 0 && st.st_mtime >= newest) {
				newest = st.st_mtime;
				strlcpy(best, path, sizeof(best));
			}
		}
		closedir(dir);
	}
	if (!best[0]) {
		sm_log("no crash report named %s*", prefix);
		return;
	}
	int in = open(best, O_RDONLY), out;
	char path[256];
	snprintf(path, sizeof(path), "/var/tmp/sweetcrash.%s.log", prefix);
	out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (in >= 0 && out >= 0) {
		static char buf[48 << 10];
		ssize_t n = read(in, buf, sizeof(buf));
		if (n > 0)
			write(out, buf, (size_t)n);
		sm_log("copied %s (%zd bytes) to %s", best, n, path);
	} else {
		sm_log("cannot copy %s", best);
	}
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
}

static void sm_watch(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = sm_signal;
	sa.sa_flags = SA_SIGINFO;
	int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGSYS };
	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
	NSSetUncaughtExceptionHandler(sm_exception);
	atexit(sm_exit);
	if (!strncmp(getprogname(), "backboardd", 10)) {
		sm_copy_report("backboardd");
		sm_copy_report("JetsamEvent");
	}
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		for (int s = 1; s <= 120; s++) {
			sleep(1);
			if (s <= 20 || s % 10 == 0) {
				uint64_t jobs = 0;
				vgpu_counters(NULL, &jobs);
				sm_log("alive +%d s, %llu GPU jobs", s, (unsigned long long)jobs);
			}
		}
	});
}

/*
 * Metal checks textures against its private MTLTextureImplementation protocol and NSLogs "texture is not a
 * MTLTextureImplementation." on every miss (metal2 adopts it too: NativeBridgeMetalFrontend.mm).
 */
static void sm_texture_protocol(void)
{
	Protocol *p = objc_getProtocol("MTLTextureImplementation");
	Class c = objc_getClass("VMTexture");
	if (p && c && !class_conformsToProtocol(c, p))
		class_addProtocol(c, p);
	sm_log("MTLTextureImplementation %s", !p ? "missing" : c && class_conformsToProtocol(c, p) ? "adopted" : "not adopted");
}

__attribute__((constructor)) static void sm_loaded(void)
{
	if (!allowed())
		return;
	sm_watch();
	sm_texture_protocol();
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
