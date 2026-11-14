/*
 * m2vtest: the metal2vulkan runtime on PanVK on the Mali-G52 (docs/METAL-PANVK.md, phase 2). The bridge's self-test
 * (bridge/NativeMetalBridge.rs, from ios-flame, MIT): a compute kernel, a render pass, a render into imported host
 * pages, then every QuartzCore kernel and vertex/fragment pair translated and built into pipelines on the phone.
 * Compute, render and import failures are recorded and the run goes on (our change to the bridge).
 *   m2vtest [DRIVER] [CACHE_DIR] [METALLIB]
 * Log: /var/tmp/sweetm2v.log; Mesa's and the runtime's messages, and one line per QuartzCore refusal (NMB_LIST), in
 * /var/tmp/sweetm2v-stderr.log. Based on ios-flame's phone/metal2/m2vtest.c (MIT).
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bridge/NativeMetalBridge.h"

/*
 * libSystem's sys_dcache_flush assumes a coherent GPU (Apple's); the Mali does not snoop the CPU caches, so it must
 * really clean and invalidate, from user space. This definition wins over libSystem's at link time. (PanVK's own
 * buffers are mapped write-combined; this matters for the host pages of the import step.)
 */
void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static double t0;
static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void mark(uint32_t m)
{
	static const char *const names[] = { "", "device", "fixture library", "compute", "render", "import", "QuartzCore" };
	unsigned i = m & 0xff;
	printf("[%7.2f s] step %x %s\n", now() - t0, m, i < 7 ? names[i] : (i == 0xf ? "done" : "?"));
	fflush(stdout);
}

static void on_alarm(int sig)
{
	(void)sig;
	printf("m2vtest: RESULT timeout (300 s) after %.0f s: the last step above hung\n", now() - t0);
	fflush(stdout);
	_exit(3);
}

static void on_crash(int sig)
{
	printf("m2vtest: RESULT crashed (signal %d) after %.0f s, in the last step above\n", sig, now() - t0);
	fflush(stdout);
	_exit(4);
}

int main(int argc, char **argv)
{
	const char *lib = argc > 1 ? argv[1] : "/usr/local/lib/libvulkan_panfrost.dylib";
	const char *cache = argc > 2 ? argv[2] : "/var/sweet/m2v-cache";
	const char *qc = argc > 3 ? argv[3] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";

	freopen("/var/tmp/sweetm2v.log", "a", stdout);
	freopen("/var/tmp/sweetm2v-stderr.log", "a", stderr);
	setvbuf(stderr, NULL, _IONBF, 0);
	signal(SIGALRM, on_alarm);
	signal(SIGSEGV, on_crash);
	signal(SIGBUS, on_crash);
	signal(SIGILL, on_crash);
	signal(SIGABRT, on_crash);
	alarm(300);
	/* as vktest: PanVK on Bifrost v7 needs both (tools/panvk-ios/vktest.c) */
	setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
	setenv("MESA_VK_VERSION_OVERRIDE", "1.2", 0);
	/* one stderr line per QuartzCore function and per refusal */
	setenv("NMB_LIST", "1", 0);
	time_t wall = time(NULL);
	printf("m2vtest: start %s", ctime(&wall));
	printf("m2vtest: driver %s, cache %s\n", lib, cache);

	mkdir("/var/sweet", 0755);
	mkdir(cache, 0777);
	int fd = open(qc, O_RDONLY);
	struct stat st;
	uint8_t *bytes = NULL;
	size_t n = 0;
	if (fd >= 0 && fstat(fd, &st) == 0) {
		n = (size_t)st.st_size;
		bytes = malloc(n);
		if (bytes == NULL || read(fd, bytes, n) != (ssize_t)n)
			n = 0;
		close(fd);
	}
	printf("QuartzCore metallib %s: %zu bytes\n", qc, n);
	fflush(stdout);
	struct NmbSelftest out = { 0 };
	t0 = now();
	uint32_t r = nmb_selftest(lib, cache, bytes, n, mark, &out);
	printf("result %x stage %x: compute bad %d/1000, render bad %d/64, import bad %d/4096 (-1: the step failed)\n", r,
	       out.stage, (int)out.compute_bad, (int)out.render_bad, (int)out.import_bad);
	printf("QuartzCore: %u functions, kernels %u ok %u failed, vertex/fragment pairs %u ok %u failed (%.1f s)\n",
	       out.qc_functions, out.qc_kernels_ok, out.qc_kernels_failed, out.qc_pairs_ok, out.qc_pairs_failed, now() - t0);
	printf("message: %.96s\n", out.message);
	printf("m2vtest: RESULT %s: compute %s, render %s, import %s, QuartzCore pipelines %u of %u\n",
	       out.stage == 0 ? "all steps ran" : "stopped", out.compute_bad == 0 ? "ok" : "WRONG",
	       out.render_bad == 0 ? "ok" : "WRONG", out.import_bad == 0 ? "ok" : "WRONG",
	       out.qc_kernels_ok + out.qc_pairs_ok, out.qc_kernels_ok + out.qc_kernels_failed + out.qc_pairs_ok + out.qc_pairs_failed);
	fflush(stdout);
	return out.stage != 0;
}
