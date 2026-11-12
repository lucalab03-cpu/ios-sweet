/*
 * libbbprobe.dylib: loaded into backboardd by bbshim. Changes nothing, only records:
 *  - that it was loaded (lib.log: LOADED ...)
 *  - every MTLCreateSystemDefaultDevice call and what it returns (nil = no Metal device: CoreAnimation
 *    draws in software)
 *  - if a device exists: which libraries, render and compute pipelines (with shader names) CoreAnimation
 *    creates. That is the real list of shaders the home screen needs.
 * After backboardd has stayed alive for 45 s it writes /var/tmp/bbprobe/ok: only then bbshim keeps loading
 * it on every start. If it crashes earlier, bbshim gives up after 2 tries and starts backboardd plain.
 *
 * Built WITHOUT ARC on purpose: the wrappers pass the +1 objects of new... methods through untouched.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define DIR "/var/tmp/bbprobe"

static int logfd = -1;
static volatile int create_calls;

static void
plog(const char *fmt, ...)
{
	char line[1024];
	int n;
	va_list ap;

	if (logfd < 0) {
		return;
	}
	n = snprintf(line, sizeof(line), "%ld ", (long)time(NULL));
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(logfd, line, n);          /* O_APPEND: one write per line, safe across threads */
}

static const char *
str(id o)
{
	const char *s = o ? [[o description] UTF8String] : "nil";
	return s ? s : "?";
}

static const char *
fname(id<MTLFunction> f)
{
	return f ? str([f name]) : "-";
}

/* ---- wrappers around the device's methods ---- */

typedef id (*rps_fn)(id, SEL, MTLRenderPipelineDescriptor *, NSError **);
typedef id (*rpso_fn)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption, MTLAutoreleasedRenderPipelineReflection *, NSError **);
typedef void (*rpsh_fn)(id, SEL, MTLRenderPipelineDescriptor *, id);
typedef id (*cps_fn)(id, SEL, id<MTLFunction>, NSError **);
typedef id (*libd_fn)(id, SEL, dispatch_data_t, NSError **);
typedef id (*libu_fn)(id, SEL, NSURL *, NSError **);
typedef id (*libb_fn)(id, SEL, NSBundle *, NSError **);
typedef id (*libs_fn)(id, SEL, NSString *, MTLCompileOptions *, NSError **);

static rps_fn o_rps;
static rpso_fn o_rpso;
static rpsh_fn o_rpsh;
static cps_fn o_cps;
static libd_fn o_libd;
static libu_fn o_libu;
static libb_fn o_libb;
static libs_fn o_libs;

static void
log_rp(const char *how, MTLRenderPipelineDescriptor *d, id result)
{
	@autoreleasepool {
		plog("RENDER-PIPELINE %s label=%s vertex=%s fragment=%s color0=%lu samples=%lu -> %s", how,
		    str([d label]), fname([d vertexFunction]), fname([d fragmentFunction]),
		    (unsigned long)[[d colorAttachments][0] pixelFormat], (unsigned long)[d rasterSampleCount],
		    result ? "ok" : "NIL");
	}
}

static id
w_rps(id self, SEL _cmd, MTLRenderPipelineDescriptor *d, NSError **e)
{
	id r = o_rps(self, _cmd, d, e);
	log_rp("sync", d, r);
	return r;
}

static id
w_rpso(id self, SEL _cmd, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *rf, NSError **e)
{
	id r = o_rpso(self, _cmd, d, o, rf, e);
	log_rp("sync+opt", d, r);
	return r;
}

static void
w_rpsh(id self, SEL _cmd, MTLRenderPipelineDescriptor *d, id handler)
{
	log_rp("async", d, @"(later)");
	o_rpsh(self, _cmd, d, handler);
}

static id
w_cps(id self, SEL _cmd, id<MTLFunction> f, NSError **e)
{
	id r = o_cps(self, _cmd, f, e);
	@autoreleasepool {
		plog("COMPUTE-PIPELINE function=%s -> %s", fname(f), r ? "ok" : "NIL");
	}
	return r;
}

static void
log_lib(const char *how, const char *what, id lib)
{
	@autoreleasepool {
		NSArray *names = lib ? [lib functionNames] : nil;
		plog("LIBRARY %s %s -> %s, %lu functions: %s", how, what, lib ? "ok" : "NIL",
		    (unsigned long)[names count], names ? [[names componentsJoinedByString:@","] UTF8String] : "");
	}
}

static id
w_libd(id self, SEL _cmd, dispatch_data_t data, NSError **e)
{
	id r = o_libd(self, _cmd, data, e);
	char what[32];
	snprintf(what, sizeof(what), "%zu-bytes", data ? dispatch_data_get_size(data) : 0);
	log_lib("data", what, r);
	return r;
}

static id
w_libu(id self, SEL _cmd, NSURL *u, NSError **e)
{
	id r = o_libu(self, _cmd, u, e);
	@autoreleasepool {
		log_lib("url", str([u path]), r);
	}
	return r;
}

static id
w_libb(id self, SEL _cmd, NSBundle *b, NSError **e)
{
	id r = o_libb(self, _cmd, b, e);
	@autoreleasepool {
		log_lib("bundle", str([b bundlePath]), r);
	}
	return r;
}

static id
w_libs(id self, SEL _cmd, NSString *src, MTLCompileOptions *opt, NSError **e)
{
	id r = o_libs(self, _cmd, src, opt, e);
	char what[32];
	snprintf(what, sizeof(what), "source-%lu-chars", (unsigned long)[src length]);
	log_lib("source", what, r);
	return r;
}

static IMP
swap(Class c, const char *sel, IMP n)
{
	Method m = class_getInstanceMethod(c, sel_registerName(sel));

	if (!m) {
		plog("  method %s not found", sel);
		return NULL;
	}
	return method_setImplementation(m, n);
}

static void
hook_device(id dev)
{
	static int done;
	Class c;

	if (!dev || done) {
		return;
	}
	done = 1;
	c = object_getClass(dev);
	plog("hooking device class %s", class_getName(c));
	o_rps = (rps_fn)swap(c, "newRenderPipelineStateWithDescriptor:error:", (IMP)w_rps);
	o_rpso = (rpso_fn)swap(c, "newRenderPipelineStateWithDescriptor:options:reflection:error:", (IMP)w_rpso);
	o_rpsh = (rpsh_fn)swap(c, "newRenderPipelineStateWithDescriptor:completionHandler:", (IMP)w_rpsh);
	o_cps = (cps_fn)swap(c, "newComputePipelineStateWithFunction:error:", (IMP)w_cps);
	o_libd = (libd_fn)swap(c, "newLibraryWithData:error:", (IMP)w_libd);
	o_libu = (libu_fn)swap(c, "newLibraryWithURL:error:", (IMP)w_libu);
	o_libb = (libb_fn)swap(c, "newDefaultLibraryWithBundle:error:", (IMP)w_libb);
	o_libs = (libs_fn)swap(c, "newLibraryWithSource:options:error:", (IMP)w_libs);
}

/* ---- MTLCreateSystemDefaultDevice, replaced through dyld interposing ---- */

static id<MTLDevice>
w_create(void)
{
	id<MTLDevice> d = MTLCreateSystemDefaultDevice();   /* inside the interposer this is the real one */

	create_calls++;
	@autoreleasepool {
		plog("MTLCreateSystemDefaultDevice #%d -> %s %s", create_calls, d ? class_getName(object_getClass(d)) : "nil",
		    d ? str([d name]) : "(no Metal device: software drawing)");
	}
	hook_device(d);
	return d;
}

__attribute__((used)) static const struct {
	const void *replacement;
	const void *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)w_create, (const void *)MTLCreateSystemDefaultDevice },
};

static void *
watchdog(void *arg)
{
	(void)arg;
	sleep(45);
	plog("ALIVE 45s, MTLCreateSystemDefaultDevice calls=%d, images=%u", create_calls, _dyld_image_count());
	FILE *f = fopen(DIR "/ok", "w");
	if (f) {
		fputs("ok\n", f);
		fclose(f);
	}
	return NULL;
}

__attribute__((constructor)) static void
bbprobe_init(void)
{
	char exe[1024];
	uint32_t len = sizeof(exe);
	pthread_t t;

	unsetenv("DYLD_INSERT_LIBRARIES");
	logfd = open(DIR "/lib.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (_NSGetExecutablePath(exe, &len) != 0) {
		exe[0] = 0;
	}
	plog("LOADED pid=%d uid=%d prog=%s exe=%s images=%u", (int)getpid(), (int)getuid(), getprogname(), exe,
	    _dyld_image_count());
	if (pthread_create(&t, NULL, watchdog, NULL) == 0) {
		pthread_detach(t);
	}
}
