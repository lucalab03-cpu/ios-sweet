/* shadertest: compiles the GLSL made from Apple's shaders with Mesa's front end, in a context on the Mali.
 * Reads /usr/local/share/sweetglsl/*.glsl, logs to /var/tmp/sweetshader.log (and stdout). */
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_PROFILE 0x33
#define OSMESA_CORE_PROFILE 0x34
#define OSMESA_COMPAT_PROFILE 0x35
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
#define OSMESA_RGBA 0x1908

static FILE *lg;
static void say(const char *fmt, ...)
{
	char l[1200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(l, sizeof(l), fmt, ap);
	va_end(ap);
	fputs(l, stdout); fflush(stdout);
	if (lg) { fputs(l, lg); fflush(lg); }
}

typedef void *(*attribs_t)(const int *, void *);
typedef void *(*create_t)(int, int, int, int, void *);
typedef int (*current_t)(void *, void *, int, int, int);
typedef void (*proc_t)(void);
typedef proc_t (*getproc_t)(const char *);

static unsigned (*glCreateShader)(unsigned);
static void (*glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*glCompileShader)(unsigned);
static void (*glGetShaderiv)(unsigned, unsigned, int *);
static void (*glGetShaderInfoLog)(unsigned, int, int *, char *);
static void (*glDeleteShader)(unsigned);
static const unsigned char *(*glGetString)(unsigned);

/* the shaders are written as GLSL 3.30; make them fit the version the context has */
static char *adapt(const char *src, int glsl_version)
{
	size_t n = strlen(src);
	char *o = malloc(n + 400), *w = o;
	const char *r = strchr(src, '\n');
	w += sprintf(w, "#version %d\n", glsl_version);
	w += sprintf(w, "#extension GL_ARB_shading_language_packing : enable\n#extension GL_ARB_shader_bit_encoding : enable\n"
	    "#extension GL_ARB_gpu_shader5 : enable\n#extension GL_ARB_explicit_attrib_location : enable\n");
	for (; r && *r; r++) {
		if (!strncmp(r, "layout(location", 15) && glsl_version < 330) {   /* drop the qualifier */
			const char *e = strchr(r, ')');
			if (e) { r = e + (e[1] == ' ' ? 1 : 0); continue; }
		}
		*w++ = *r;
	}
	*w = 0;
	return o;
}

int main(void)
{
	lg = fopen("/var/tmp/sweetshader.log", "w");
	say("shadertest: start\n");
	/* panfrost reports GL 2.1 for OSMesa; ask Mesa for the GL 3.3 the Mali can mostly do */
	setenv("MESA_GL_VERSION_OVERRIDE", "3.3", 1);
	setenv("MESA_GLSL_VERSION_OVERRIDE", "330", 1);
	void *lib = dlopen("/usr/local/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!lib) { say("dlopen: %s\n", dlerror()); return 1; }
	attribs_t attribs = (attribs_t)dlsym(lib, "OSMesaCreateContextAttribs");
	create_t create = (create_t)dlsym(lib, "OSMesaCreateContextExt");
	current_t current = (current_t)dlsym(lib, "OSMesaMakeCurrent");
	getproc_t getproc = (getproc_t)dlsym(lib, "OSMesaGetProcAddress");
	static uint32_t px[64 * 64];
	void *ctx = NULL;
	static const int vers[][3] = { {3, 3, OSMESA_CORE_PROFILE}, {3, 2, OSMESA_CORE_PROFILE}, {3, 1, OSMESA_COMPAT_PROFILE} };
	for (int i = 0; attribs && i < 3 && !ctx; i++) {
		int a[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_PROFILE, vers[i][2],
		    OSMESA_CONTEXT_MAJOR_VERSION, vers[i][0], OSMESA_CONTEXT_MINOR_VERSION, vers[i][1], 0 };
		ctx = attribs(a, NULL);
		say("context %d.%d: %s\n", vers[i][0], vers[i][1], ctx ? "ok" : "no");
	}
	if (!ctx) ctx = create(OSMESA_RGBA, 0, 0, 0, NULL);
	if (!ctx || !current(ctx, px, 0x1401, 64, 64)) { say("no context\n"); return 2; }
#define L(n) *(proc_t *)&n = getproc(#n)
	L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv); L(glGetShaderInfoLog);
	L(glDeleteShader); L(glGetString);
	const char *ver = (const char *)glGetString(GL_VERSION), *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	say("GL %s | GLSL %s | %s\n", ver, glsl, glGetString(GL_RENDERER));
	int v = 330;
	{ int a = 0, b = 0; if (glsl && sscanf(glsl, "%d.%d", &a, &b) == 2) v = a * 100 + b; if (v >= 400) v = 330; }
	say("compiling as GLSL %d\n", v);
	DIR *d = opendir("/usr/local/share/sweetglsl");
	struct dirent *e;
	int ok = 0, bad = 0;
	while (d && (e = readdir(d))) {
		char path[300], *buf; FILE *f; long len;
		if (!strstr(e->d_name, ".glsl")) continue;
		snprintf(path, sizeof(path), "/usr/local/share/sweetglsl/%s", e->d_name);
		f = fopen(path, "rb"); if (!f) continue;
		fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
		buf = calloc(1, len + 1); fread(buf, 1, len, f); fclose(f);
		char *src = adapt(buf, v);
		unsigned sh = glCreateShader(strstr(e->d_name, "_vert") ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
		const char *p = src;
		glShaderSource(sh, 1, &p, NULL);
		glCompileShader(sh);
		int st = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
		if (st) { ok++; say("OK   %s\n", e->d_name); }
		else {
			char info[500] = ""; glGetShaderInfoLog(sh, sizeof(info) - 1, NULL, info);
			for (char *q = info; *q; q++) if (*q == '\n') *q = ' ';
			bad++; say("FAIL %s: %.300s\n", e->d_name, info);
		}
		glDeleteShader(sh); free(src); free(buf);
	}
	if (d) closedir(d);
	say("RESULT: %d compiled, %d failed\n", ok, bad);
	return 0;
}
