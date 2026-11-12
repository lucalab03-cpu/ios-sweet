/* drawtest: draws with two translated Apple shaders (std_vert0 + axial_shading_frag) on the Mali and checks the pixels
 * against the same calculation done on the CPU. Log: /var/tmp/sweetdraw.log */
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 5
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x4000
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_PROFILE 0x33
#define OSMESA_CORE_PROFILE 0x34
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
#define OSMESA_RGBA 0x1908
#define W 64
#define H 64

static FILE *lg;
static void say(const char *fmt, ...)
{
	char l[600]; va_list ap;
	va_start(ap, fmt); vsnprintf(l, sizeof(l), fmt, ap); va_end(ap);
	fputs(l, stdout); fflush(stdout);
	if (lg) { fputs(l, lg); fflush(lg); }
}

typedef void (*proc_t)(void);
typedef proc_t (*getproc_t)(const char *);
static unsigned (*glCreateShader)(unsigned);
static void (*glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*glCompileShader)(unsigned);
static void (*glGetShaderiv)(unsigned, unsigned, int *);
static void (*glGetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (*glCreateProgram)(void);
static void (*glAttachShader)(unsigned, unsigned);
static void (*glLinkProgram)(unsigned);
static void (*glGetProgramiv)(unsigned, unsigned, int *);
static void (*glGetProgramInfoLog)(unsigned, int, int *, char *);
static void (*glUseProgram)(unsigned);
static int (*glGetUniformLocation)(unsigned, const char *);
static void (*glUniform4fv)(int, int, const float *);
static void (*glUniform1i)(int, int);
static void (*glGenVertexArrays)(int, unsigned *);
static void (*glBindVertexArray)(unsigned);
static void (*glGenBuffers)(int, unsigned *);
static void (*glBindBuffer)(unsigned, unsigned);
static void (*glBufferData)(unsigned, long, const void *, unsigned);
static void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (*glEnableVertexAttribArray)(unsigned);
static void (*glDrawArrays)(unsigned, int, int);
static void (*glGenTextures)(int, unsigned *);
static void (*glBindTexture)(unsigned, unsigned);
static void (*glActiveTexture)(unsigned);
static void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*glTexParameteri)(unsigned, unsigned, int);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(unsigned);
static void (*glViewport)(int, int, int, int);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *);

static char *slurp(const char *name)
{
	char path[300]; FILE *f; long n; char *b;
	snprintf(path, sizeof(path), "/usr/local/share/sweetglsl/%s", name);
	f = fopen(path, "rb"); if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = calloc(1, n + 1); fread(b, 1, n, f); fclose(f);
	return b;
}

static unsigned compile(unsigned kind, const char *name)
{
	char *src = slurp(name); unsigned sh; int st = 0; char info[400] = "";
	const char *p = src;
	if (!src) { say("cannot read %s\n", name); return 0; }
	sh = glCreateShader(kind); glShaderSource(sh, 1, &p, NULL); glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
	if (!st) { glGetShaderInfoLog(sh, sizeof(info) - 1, NULL, info); say("compile %s failed: %.300s\n", name, info); return 0; }
	return sh;
}

static int fails;
static void check(const char *what, const uint8_t *got, const float *want)
{
	int ok = 1;
	for (int i = 0; i < 4; i++) { int w = (int)lrintf(want[i] * 255.0f); if (abs((int)got[i] - w) > 2) ok = 0; }
	say("%s: got %d,%d,%d,%d want %.0f,%.0f,%.0f,%.0f -> %s\n", what, got[0], got[1], got[2], got[3],
	    want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255, ok ? "OK" : "WRONG");
	if (!ok) fails++;
}

static uint8_t px[W * H * 4];

static void draw(unsigned prog, float scale, float lo, float hi, const float *edge_color)
{
	float m[16] = { scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };   /* buf3: four columns */
	float u[8] = { lo, hi, 0, 0, edge_color[0], edge_color[1], edge_color[2], edge_color[3] };       /* buf5 */
	float t[4] = { 0, 0, 0, 0 };
	glUniform4fv(glGetUniformLocation(prog, "buf3"), 4, m);
	glUniform4fv(glGetUniformLocation(prog, "buf4"), 1, t);
	glUniform4fv(glGetUniformLocation(prog, "buf5"), 2, u);
	glClearColor(0.1f, 0.2f, 0.3f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	memset(px, 0, sizeof(px));
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

int main(void)
{
	lg = fopen("/var/tmp/sweetdraw.log", "w");
	say("drawtest: start\n");
	setenv("MESA_GL_VERSION_OVERRIDE", "3.3", 1);
	setenv("MESA_GLSL_VERSION_OVERRIDE", "330", 1);
	void *lib = dlopen("/usr/local/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!lib) { say("dlopen: %s\n", dlerror()); return 1; }
	void *(*attribs)(const int *, void *) = dlsym(lib, "OSMesaCreateContextAttribs");
	int (*current)(void *, void *, int, int, int) = dlsym(lib, "OSMesaMakeCurrent");
	getproc_t getproc = (getproc_t)dlsym(lib, "OSMesaGetProcAddress");
	int a[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_PROFILE, OSMESA_CORE_PROFILE,
	    OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 3, 0 };
	void *ctx = attribs ? attribs(a, NULL) : NULL;
	if (!ctx || !current(ctx, px, GL_UNSIGNED_BYTE, W, H)) { say("no 3.3 context\n"); return 2; }
#define L(n) *(proc_t *)&n = getproc(#n)
	L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv); L(glGetShaderInfoLog);
	L(glCreateProgram); L(glAttachShader); L(glLinkProgram); L(glGetProgramiv); L(glGetProgramInfoLog); L(glUseProgram);
	L(glGetUniformLocation); L(glUniform4fv); L(glUniform1i); L(glGenVertexArrays); L(glBindVertexArray);
	L(glGenBuffers); L(glBindBuffer); L(glBufferData); L(glVertexAttribPointer); L(glEnableVertexAttribArray);
	L(glDrawArrays); L(glGenTextures); L(glBindTexture); L(glActiveTexture); L(glTexImage2D); L(glTexParameteri);
	L(glClearColor); L(glClear); L(glViewport); L(glFinish); L(glReadPixels);

	unsigned vs = compile(GL_VERTEX_SHADER, "std_vert0_lpf.glsl"), fs = compile(GL_FRAGMENT_SHADER, "axial_shading_frag_lpf.glsl");
	if (!vs || !fs) return 3;
	unsigned prog = glCreateProgram(); int st = 0;
	glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &st);
	if (!st) { char info[400] = ""; glGetProgramInfoLog(prog, sizeof(info) - 1, NULL, info); say("link failed: %.300s\n", info); return 4; }
	say("linked std_vert0 + axial_shading_frag\n");
	glUseProgram(prog);

	/* a full-screen quad: position (vec4), texcoord (vec2), color (vec4) */
	const float col[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
	float v[4][10] = {
		{ -1, -1, 0, 1, 0, 0, col[0], col[1], col[2], col[3] }, { 1, -1, 0, 1, 0, 0, col[0], col[1], col[2], col[3] },
		{ -1, 1, 0, 1, 0, 0, col[0], col[1], col[2], col[3] }, { 1, 1, 0, 1, 0, 0, col[0], col[1], col[2], col[3] } };
	unsigned vao, vbo, tex;
	glGenVertexArrays(1, &vao); glBindVertexArray(vao);
	glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, 0, 40, (void *)0);  glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 40, (void *)16); glEnableVertexAttribArray(1);
	glVertexAttribPointer(3, 4, GL_FLOAT, 0, 40, (void *)24); glEnableVertexAttribArray(3);
	const uint8_t texel[4] = { 51, 102, 204, 255 };                 /* 0.2, 0.4, 0.8, 1.0 */
	glActiveTexture(GL_TEXTURE0); glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(glGetUniformLocation(prog, "tex3"), 0);
	glViewport(0, 0, W, H);

	const float edge[4] = { 0.5f, 0.25f, 1.0f, 1.0f };
	/* 1: texcoord x = 0 is inside [-1, 1]  -> the texture colour times the vertex colour */
	draw(prog, 1.0f, -1.0f, 1.0f, edge);
	{ float want[4] = { 0.2f * col[0], 0.4f * col[1], 0.8f * col[2], 1.0f }; check("inside the range (texture)", px + (32 * W + 32) * 4, want); }
	/* 2: texcoord x = 0 is outside [0.25, 0.75] -> the edge colour times the vertex colour */
	draw(prog, 1.0f, 0.25f, 0.75f, edge);
	{ float want[4] = { edge[0] * col[0], edge[1] * col[1], edge[2] * col[2], edge[3] * col[3] }; check("outside the range (edge colour)", px + (32 * W + 32) * 4, want); }
	/* 3: the matrix scales the quad to half size: the corner stays the clear colour, the centre is drawn */
	draw(prog, 0.5f, 0.25f, 0.75f, edge);
	{ float want[4] = { 0.1f, 0.2f, 0.3f, 1.0f }; check("half-size quad: corner (clear colour)", px + (2 * W + 2) * 4, want); }
	{ float want[4] = { edge[0] * col[0], edge[1] * col[1], edge[2] * col[2], 1.0f }; check("half-size quad: centre", px + (32 * W + 32) * 4, want); }
	say("RESULT: %s\n", fails ? "some pixels are wrong" : "the translated shaders draw what they should");
	return fails ? 5 : 0;
}
