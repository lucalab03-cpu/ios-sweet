/*
 * SweetMTLDriver.bundle: the Metal driver that iOS 16's Metal loads for our GPU.
 *
 * How Metal finds it (_MTLRegisterDevices, see ios-viva docs/PIANO-GPU.md "Fatti verificati"): every IOAcceleratorES
 * service in the kernel that has MetalPluginName loads /System/Library/Extensions/<MetalPluginName>.bundle, takes
 * the class named by MetalPluginClassName, checks that it is a subclass of _MTLDevice, then calls
 * [[cls alloc] initWithAcceleratorPort:service]. Our kernel (patch 0126) publishes SweetAccelerator with
 * MetalPluginName = SweetMTLDriver and MetalPluginClassName = SweetMTLDevice.
 *
 * Only the programs listed in /var/sweet/metal.allow (one name per line; without the file: metaltest) get a device.
 * For every other process, backboardd and SpringBoard first of all, +alloc returns nil: no object is ever made, so
 * nothing has to be torn down (boot 2026-11-14: returning nil from init let ARC release a never-initialised
 * _MTLDevice, whose dealloc crashed backboardd in a loop).
 *
 * In allowed processes every method of _MTLDevice that this class does not implement itself is routed through
 * -forwardInvocation:, which logs it and then runs _MTLDevice's own implementation. A SIGABRT handler logs the
 * backtrace. Together they show what Metal asks of a device before it gives up.
 * Log: /var/tmp/sweetmtl.log
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Metal's private base class of every device; resolved at load time from Metal.framework (link flag -U). */
@interface _MTLDevice : NSObject
@end

@interface SweetMTLDevice : _MTLDevice
- (instancetype)initWithAcceleratorPort:(unsigned int)port;
@end

/* Foundation has it, the SDK headers do not declare it. */
@interface NSInvocation (SweetPrivate)
- (void)invokeUsingIMP:(IMP)imp;
@end

static void say(const char *fmt, ...)
{
	char line[700];
	va_list ap;
	FILE *f = fopen("/var/tmp/sweetmtl.log", "a");
	if (!f) return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(f, "%s[%d]: %s\n", getprogname(), (int)getpid(), line);
	fclose(f);
}

/* Is this process allowed to get the device? Decided once. */
static int allowed(void)
{
	static int answer = -1;
	if (answer >= 0) return answer;
	const char *me = getprogname();
	FILE *f = fopen("/var/sweet/metal.allow", "r");
	char line[256];
	int yes = 0;

	if (!f) yes = strcmp(me, "metaltest") == 0;
	else {
		while (!yes && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n \t")] = 0;
			if (line[0] && strcmp(line, me) == 0) yes = 1;
		}
		fclose(f);
	}
	answer = yes;
	return answer;
}

/* SIGABRT: write the backtrace (image + offset + nearest symbol) before the process goes. */
static void on_abort(int sig)
{
	void *frames[48];
	int n = backtrace(frames, 48);
	say("SIGABRT, %d frames:", n);
	for (int i = 0; i < n; i++) {
		Dl_info info;
		if (dladdr(frames[i], &info) && info.dli_fname) {
			const char *img = strrchr(info.dli_fname, '/');
			say("  #%d %p %s+0x%lx %s+0x%lx", i, frames[i], img ? img + 1 : info.dli_fname,
			    (unsigned long)((char *)frames[i] - (char *)info.dli_fbase),
			    info.dli_sname ? info.dli_sname : "?",
			    info.dli_saddr ? (unsigned long)((char *)frames[i] - (char *)info.dli_saddr) : 0ul);
		} else say("  #%d %p", i, frames[i]);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/* Selectors never routed through the trace: object lifetime and the runtime's own machinery. */
static int skip_selector(const char *n)
{
	static const char *skip[] = { "dealloc", ".cxx_destruct", ".cxx_construct", "init", "initWithAcceleratorPort:",
		"retain", "release", "autorelease", "retainCount", "_tryRetain", "_isDeallocating", "allowsWeakReference",
		"retainWeakReference", "description", "debugDescription", "class", "isKindOfClass:", "respondsToSelector:",
		"methodSignatureForSelector:", "forwardInvocation:", "conformsToProtocol:", "name", NULL };
	for (int i = 0; skip[i]; i++) if (!strcmp(n, skip[i])) return 1;
	return 0;
}

/* Route every _MTLDevice method this class does not implement through -forwardInvocation:. */
static void install_trace(Class mine)
{
	Class base = objc_getClass("_MTLDevice");
	unsigned int count = 0, added = 0;
	Method *list = base ? class_copyMethodList(base, &count) : NULL;
	for (unsigned int i = 0; i < count; i++) {
		SEL s = method_getName(list[i]);
		const char *n = sel_getName(s);
		if (skip_selector(n)) continue;
		if (class_addMethod(mine, s, (IMP)_objc_msgForward, method_getTypeEncoding(list[i]))) added++;
	}
	free(list);
	say("trace: %u methods of _MTLDevice, %u routed through the log", count, added);
}

__attribute__((constructor)) static void loaded(void)
{
	say("bundle loaded%s", allowed() ? " (allowed: tracing)" : "");
	if (allowed()) {
		/* MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] (boot 2026-11-14). _MTLDevice's
		 * methods are all there at runtime; the conformance is declared at runtime so the compiler does not ask for
		 * every MTLDevice method in this file. */
		BOOL ok = class_addProtocol(objc_getClass("SweetMTLDevice"), @protocol(MTLDevice));
		say("MTLDevice protocol: %s", ok ? "added" : "already there or refused");
		/* Then it asserts [newDevice conformsToProtocol:@protocol(MTLDeviceSPI)] (line 835, boot 2026-11-14): the
		 * private protocol, which Metal's own image defines. Every other MTL*Device* protocol is logged, for the
		 * next assertion. */
		unsigned int np = 0;
		Protocol *__unsafe_unretained *all = objc_copyProtocolList(&np);
		for (unsigned int i = 0; i < np; i++) {
			const char *pn = protocol_getName(all[i]);
			if (strncmp(pn, "MTL", 3) != 0 || strstr(pn, "Device") == NULL) continue;
			if (!strcmp(pn, "MTLDeviceSPI")) {
				BOOL spi = class_addProtocol(objc_getClass("SweetMTLDevice"), all[i]);
				say("protocol %s: %s", pn, spi ? "added" : "already there or refused");
			} else {
				say("protocol %s: seen", pn);
			}
		}
		free(all);
		if (!objc_getProtocol("MTLDeviceSPI")) say("protocol MTLDeviceSPI: not found in the runtime");
		signal(SIGABRT, on_abort);
		install_trace(objc_getClass("SweetMTLDevice"));
	}
}

@implementation SweetMTLDevice

+ (instancetype)alloc
{
	return [self allocWithZone:NULL];
}

+ (instancetype)allocWithZone:(struct _NSZone *)zone
{
	if (!allowed()) {
		say("alloc: not on the allow list, no device");
		return nil;
	}
	return [super allocWithZone:zone];
}

- (instancetype)initWithAcceleratorPort:(unsigned int)port
{
	self = [super init];
	say("initWithAcceleratorPort: service port 0x%x, device %p", port, (__bridge void *)self);
	return self;
}

- (NSString *)name
{
	say("call name");
	return @"Sweet Mali-G52 (stub)";
}

/*
 * MTLAddDevice runs these four on every device it registers. _MTLDevice's own versions fill the limits and feature
 * tables from an Apple GPU's hardware profile: its featureProfile has none here and recursed until the stack ran out
 * (metaltest, boot 2026-11-14: 2416 calls). Empty, as in ios-flame's frontend (NativeBridgeMetalFrontend.mm): every
 * limit and feature stays the base class's neutral 0/NO until this driver answers it itself.
 */
- (void)initLimits { say("call initLimits (empty)"); }
- (void)initFeatureQueries { say("call initFeatureQueries (empty)"); }
- (void)initGPUFamilySupport { say("call initGPUFamilySupport (empty)"); }
- (void)initWorkarounds { say("call initWorkarounds (empty)"); }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set { (void)set; return NO; }

- (void)forwardInvocation:(NSInvocation *)inv
{
	SEL s = inv.selector;
	IMP impl = class_getMethodImplementation(objc_getClass("_MTLDevice"), s);
	say("call %s", sel_getName(s));
	[inv invokeUsingIMP:impl];
	say("ret  %s", sel_getName(s));
}

@end
