// SPDX-License-Identifier: MIT
/*
 * ios-sweet: CoreAnimation's "function" numbers for eagl.c, from inside QuartzCore 16.7.16 (20H392, iPhone10,3).
 *
 * Taken from the Galaxy S7 port (https://github.com/wardevsa/ios-a5xelte, branch herolte,
 * gpu/hakios-gl/src/ca_hooks.c, MIT License, Copyright (c) 2026 erikkunz), itself the part of HakiOS's
 * src/inject.c (MIT, Copyright (c) 2025-2026 the HakiOS authors) that eagl.c needs. The addresses of the 20H392
 * cache are theirs and pixel3-ios's (gpu/eagl-ios/src/blueeagl.c, the same three numbers). Changes for ios-sweet:
 * only the 20H392 entry, the names eagl.c has here (redwood_gl_*), backboardd.orig, a log file of its own
 * (/var/tmp/sweetca.log) and the switches read from /var/sweet/glargs (glhooks=0, glsb3=1).
 *
 * Why (round an, 2026-11-26): CA::OGL::GLESContext::function_uniform() hands a draw's function its numbers (a colour
 * matrix, a blur's places). For the functions CoreAnimation's OpenGL ES renderer has no shader for, it draws with no
 * program and the numbers go nowhere; eagl.c draws those with programs of its own (plain_draw_begin,
 * group_draw_begin) but only when it knows the function, which it reads through the context this hook hands it.
 * Without it every such draw was a plain picture: Control Center's round buttons came out as white discs (the
 * gl-at trace of round am/an: a white circle group composited with program 0) and, on the S7, "the lock screen's
 * vibrancy gradient came out as a red veil over the wallpaper" -- ours is a red-to-black gradient.
 *
 * Both virtual-table entries are checked against the expected originals before they are written; on any other
 * cache nothing is touched and the log says so.
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
void redwood_gl_function_values(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);
void redwood_gl_function_refused(unsigned function);

#define CACHE_BASE              0x180000000UL
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

/* QuartzCore 16.7.16 (20H392), unslid: vtable for CA::OGL::GLESContext; +0x378 GLContext::function_supported(),
 * +0x388 GLContext::function_uniform(); the table of shader sources by function */
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL
#define EXPECT_FUNCTION_SUPP    0x1884b2164UL
#define EXPECT_FUNCTION_UNIF    0x1884b21acUL
#define CA_SHADER_TABLE         0x1d5b48a10UL
#define FUNCTION_SHADOW_BLUR    40

static uintptr_t slide;
static int log_fd = -1;

static void
say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	if (log_fd < 0) {
		log_fd = open("/var/tmp/sweetca.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (log_fd < 0) {
			return;
		}
	}
	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(log_fd, line, (size_t)n);
}

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if ((*entry & 0x7fffffffffUL) != expected + slide) {
		say("sweet-ca: %s: found %#lx, expected %#lx; left alone\n", what, (unsigned long)(*entry - slide),
		    (unsigned long)expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("sweet-ca: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)(expected + slide);
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/*
 * glsb3=1 in /var/sweet/glargs: function 40 ("sb3", the four samples of a small shadow blur), for which the renderer
 * has no shader, is given one in its table of sources (HakiOS inject.c: ds4's shader without the weights) and
 * function_supported() says yes for it. Without it, what needs the function is not drawn.
 */
static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static int shadow_blur_given;

static void
give_shadow_blur(void)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)FUNCTION_SHADOW_BLUR);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	/* ds4 (26) must be where it was found: the table is the one this cache has */
	if (*(uintptr_t *)(CA_SHADER_TABLE + slide + 8 * 26) == 0 || *entry != 0) {
		say("sweet-ca: the shader table is not as expected; sb3 left alone\n");
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("sweet-ca: cannot make the table of shaders writable\n");
		return;
	}
	*entry = (uintptr_t)shadow_blur_source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	shadow_blur_given = 1;
	say("sweet-ca: CoreAnimation's function 40 (\"sb3\") is given a shader of ours\n");
}

static int (*orig_function_supported)(void *context);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function == FUNCTION_SHADOW_BLUR && shadow_blur_given) {
		return 1;
	}
	if (!supported) {
		redwood_gl_function_refused(function);
		if (!said[function]) {
			said[function] = 1;
			say("sweet-ca: CoreAnimation's OpenGL ES renderer has no function %u: what needs it is not drawn\n", function);
		}
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	redwood_gl_function_values(context, function, index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

/* the switches of /var/sweet/glargs, read here: this runs before eagl.c reads them */
static int
glargs_has(const char *word)
{
	char text[1024];
	int fd = open("/var/sweet/glargs", O_RDONLY);
	ssize_t got = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;

	if (fd >= 0) {
		close(fd);
	}
	if (got <= 0) {
		return 0;
	}
	text[got] = 0;
	return strstr(text, word) != NULL;
}

__attribute__((constructor)) static void
sweet_ca_hooks(void)
{
	size_t length = 0;
	const void *cache;

	if (strncmp(getprogname(), "backboardd", 10) != 0 || (cache = _dyld_get_shared_cache_range(&length)) == NULL) {
		return;
	}
	if (glargs_has("glhooks=0")) {
		say("sweet-ca: %s[%d]: glhooks=0, CoreAnimation left alone\n", getprogname(), getpid());
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	uintptr_t s = *(uintptr_t *)(VSLOT(VT_GLES_CONTEXT, 0x378) + slide) & 0x7fffffffffUL;
	uintptr_t u = *(uintptr_t *)(VSLOT(VT_GLES_CONTEXT, 0x388) + slide) & 0x7fffffffffUL;

	if (s != EXPECT_FUNCTION_SUPP + slide || u != EXPECT_FUNCTION_UNIF + slide) {
		say("sweet-ca: %s[%d]: QuartzCore is not 20H392 as known here (%#lx, %#lx; slide %#lx): no hooks\n", getprogname(),
		    getpid(), (unsigned long)(s - slide), (unsigned long)(u - slide), (unsigned long)slide);
		return;
	}
	if (glargs_has("glsb3=1")) {
		give_shadow_blur();
	}
	orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), EXPECT_FUNCTION_SUPP,
	    my_function_supported);
	orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), EXPECT_FUNCTION_UNIF,
	    my_function_uniform);
	say("sweet-ca: %s[%d]: QuartzCore 20H392: function_supported %s, function_uniform %s (slide %#lx)\n", getprogname(),
	    getpid(), orig_function_supported ? "watched" : "NOT watched", orig_function_uniform ? "watched" : "NOT watched",
	    (unsigned long)slide);
}
