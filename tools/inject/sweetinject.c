/* Inserted into backboardd: logs that it got in, what Metal looks like from inside, and (not more than twice) asks Metal
 * for its default device. Log: /var/tmp/sweetinject.log */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void say(const char *fmt, ...)
{
	char l[700]; va_list ap;
	FILE *f = fopen("/var/tmp/sweetinject.log", "a");
	if (!f) return;
	va_start(ap, fmt); vsnprintf(l, sizeof(l), fmt, ap); va_end(ap);
	fputs(l, f); fclose(f);
}

static int launches;

static void *probe(void *arg)
{
	(void)arg;
	sleep(25);
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		if (strstr(n, "Metal") || strstr(n, "QuartzCore") || strstr(n, "IOSurface") || strstr(n, "AGX") || strstr(n, "IOAccel"))
			say("inject: image %s\n", n);
	}
	void *h = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
	say("inject: dlopen Metal: %s\n", h ? "ok" : dlerror());
	void *(*create)(void) = h ? dlsym(h, "MTLCreateSystemDefaultDevice") : NULL;
	say("inject: MTLCreateSystemDefaultDevice: %p\n", (void *)create);
	if (create && launches <= 2) {
		say("inject: calling it\n");
		void *dev = create();
		say("inject: default device: %p\n", dev);
	} else say("inject: not calling it (launch %d)\n", launches);
	return NULL;
}

__attribute__((constructor)) static void init(void)
{
	char path[1024] = "?"; uint32_t sz = sizeof(path);
	FILE *c = fopen("/var/tmp/sweetinject.count", "r");
	if (c) { if (fscanf(c, "%d", &launches) != 1) launches = 0; fclose(c); }
	launches++;
	c = fopen("/var/tmp/sweetinject.count", "w"); if (c) { fprintf(c, "%d\n", launches); fclose(c); }
	_NSGetExecutablePath(path, &sz);
	say("inject: loaded in pid %d uid %d exe %s (launch %d)\n", (int)getpid(), (int)getuid(), path, launches);
	pthread_t t;
	if (pthread_create(&t, NULL, probe, NULL) == 0) pthread_detach(t);
}
