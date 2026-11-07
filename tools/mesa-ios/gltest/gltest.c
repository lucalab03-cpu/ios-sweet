/*
 * Draws a triangle with OpenGL on the Mali through Mesa's OSMesa and panfrost, and reads it back.
 * Everything is found with dlopen: nothing here links against Mesa. Output goes to stdout and
 * /var/tmp/gltest.log.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 256
#define H 256
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 4
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define OSMESA_RGBA 0x1908

static FILE *logf_;
static void
say(const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fflush(stdout);
	if (logf_) {
		fputs(line, logf_);
		fflush(logf_);
	}
}

typedef void *(*create_t)(int format, int depth, int stencil, int accum, void *share);
typedef int (*current_t)(void *ctx, void *buffer, int type, int w, int h);
typedef void (*proc_t)(void);
typedef proc_t (*getproc_t)(const char *);

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/usr/local/lib/libOSMesa.8.dylib";
	static uint32_t pixels[W * H];
	void *lib, *ctx;
	uint32_t centre, corner;

	logf_ = fopen("/var/tmp/gltest.log", "w");
	say("gltest: start, library %s\n", path);
	lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	if (!lib) {
		say("gltest: dlopen failed: %s\n", dlerror());
		return 1;
	}
	create_t create = (create_t)dlsym(lib, "OSMesaCreateContextExt");
	current_t current = (current_t)dlsym(lib, "OSMesaMakeCurrent");
	getproc_t getproc = (getproc_t)dlsym(lib, "OSMesaGetProcAddress");
	if (!create || !current || !getproc) {
		say("gltest: OSMesa symbols missing\n");
		return 2;
	}
	ctx = create(OSMESA_RGBA, 0, 0, 0, NULL);
	if (!ctx || !current(ctx, pixels, GL_UNSIGNED_BYTE, W, H)) {
		say("gltest: no OSMesa context\n");
		return 3;
	}
	const unsigned char *(*getstr)(unsigned) = (const unsigned char *(*)(unsigned))getproc("glGetString");
	void (*clearcolor)(float, float, float, float) = (void (*)(float, float, float, float))getproc("glClearColor");
	void (*clear)(unsigned) = (void (*)(unsigned))getproc("glClear");
	void (*begin)(unsigned) = (void (*)(unsigned))getproc("glBegin");
	void (*end)(void) = (void (*)(void))getproc("glEnd");
	void (*color3f)(float, float, float) = (void (*)(float, float, float))getproc("glColor3f");
	void (*vertex2f)(float, float) = (void (*)(float, float))getproc("glVertex2f");
	void (*finish)(void) = (void (*)(void))getproc("glFinish");
	void (*viewport)(int, int, int, int) = (void (*)(int, int, int, int))getproc("glViewport");
	void (*readpixels)(int, int, int, int, unsigned, unsigned, void *) =
	    (void (*)(int, int, int, int, unsigned, unsigned, void *))getproc("glReadPixels");

	say("gltest: renderer: %s\n", getstr(GL_RENDERER));
	say("gltest: version: %s\n", getstr(GL_VERSION));
	viewport(0, 0, W, H);
	clearcolor(0.f, 0.f, 1.f, 1.f);
	clear(GL_COLOR_BUFFER_BIT);
	begin(GL_TRIANGLES);
	color3f(1.f, 0.f, 0.f); vertex2f(-0.8f, -0.8f);
	color3f(0.f, 1.f, 0.f); vertex2f(0.8f, -0.8f);
	color3f(1.f, 1.f, 1.f); vertex2f(0.f, 0.8f);
	end();
	finish();
	memset(pixels, 0, sizeof(pixels));
	readpixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	centre = pixels[(H / 2) * W + W / 2];
	corner = pixels[0];
	say("gltest: centre pixel %08x, corner pixel %08x\n", centre, corner);
	/* byte order is R,G,B,A in memory: the corner is the blue clear colour, the centre is not */
	if (corner == 0xffff0000u && centre != corner) {
		say("RESULT: OpenGL drew the triangle\n");
		return 0;
	}
	say("RESULT: the picture is not what was drawn\n");
	return 4;
}
