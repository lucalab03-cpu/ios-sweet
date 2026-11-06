/*
 * malitest: the first program to use the Mali-G52 through the SweetMali service.
 *
 * It opens the service, asks the GPU's parameters, makes a buffer, maps it, writes a "write value" job
 * chain into it, submits the chain and reads back the word the GPU wrote. Every step is printed to
 * standard output and appended to /var/tmp/malitest.log, so the result can be read from the phone's
 * disk even if nothing else shows it. The requests are the ones of Linux's panfrost DRM driver (see
 * iokit/Kernel/arm/SweetMali.cpp): this is what Mesa's panfrost will do, in miniature.
 *
 * Build (CI, macOS): xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O1 \
 *     malitest.c -framework IOKit -o malitest && codesign -s - --force malitest
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <mach/mach.h>

/* IOKitLib's own header is not in the iOS SDK: the few calls used here. */
typedef mach_port_t io_object_t, io_service_t, io_connect_t, io_iterator_t;
extern const mach_port_t kIOMasterPortDefault;
extern kern_return_t IOMasterPort(mach_port_t, mach_port_t *);
extern io_service_t IOServiceGetMatchingService(mach_port_t, void * matching);
extern void * IOServiceMatching(const char * name);
extern kern_return_t IOServiceOpen(io_service_t, task_t, uint32_t type, io_connect_t *);
extern kern_return_t IOServiceClose(io_connect_t);
extern kern_return_t IOObjectRelease(io_object_t);
extern kern_return_t IOConnectCallStructMethod(io_connect_t, uint32_t selector,
    const void * in, size_t inSize, void * out, size_t * outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t, uint32_t type, task_t, mach_vm_address_t * address,
    mach_vm_size_t * size, uint32_t options);
#define SWEET_ANYWHERE 1u

struct create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct mmap_bo { uint32_t handle, flags; uint64_t offset; };
struct get_param { uint32_t param, pad; uint64_t value; };
struct get_bo_offset { uint32_t handle, pad; uint64_t offset; };
struct submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct gem_close { uint32_t handle, pad; };

static FILE * logfile;

static void
say(const char * fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("MALITEST: %s\n", line);
	fflush(stdout);
	if (logfile) {
		fprintf(logfile, "%s\n", line);
		fflush(logfile);
	}
}

static kern_return_t
request(io_connect_t c, uint32_t selector, void * data, size_t size)
{
	size_t out = size;

	return IOConnectCallStructMethod(c, selector, data, size, data, &out);
}

int
main(void)
{
	io_service_t service;
	io_connect_t conn = 0;
	kern_return_t kr;
	int failures = 0;

	logfile = fopen("/var/tmp/malitest.log", "a");
	say("start, pid %d", getpid());

	/* the kernel brings the GPU up some 30 s after boot (sweetmali=3) and only then publishes the service */
	service = 0;
	for (int tries = 0; tries < 150 && !service; tries++) {
		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetMali"));
		if (!service) {
			if (tries % 10 == 0) {
				say("waiting for the SweetMali service (%d s)", tries * 2);
			}
			sleep(2);
		}
	}
	if (!service) {
		say("FAIL: no SweetMali service after 300 s (boot with sweetmali=3)");
		return 1;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	if (kr != KERN_SUCCESS) {
		say("FAIL: IOServiceOpen %#x", kr);
		return 1;
	}
	say("opened the service");

	static const struct { uint32_t id; const char * name; } params[] = {
		{ 0, "GPU_PROD_ID" }, { 1, "GPU_REVISION" }, { 2, "SHADER_PRESENT" }, { 3, "TILER_PRESENT" },
		{ 4, "L2_PRESENT" }, { 6, "L2_FEATURES" }, { 13, "THREAD_MAX_THREADS" }, { 39, "THREAD_TLS_ALLOC" },
		{ 41, "SYSTEM_TIMESTAMP" }, { 42, "TIMESTAMP_FREQUENCY" },
	};
	for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct get_param p = { .param = params[i].id };

		kr = request(conn, 0x44, &p, sizeof(p));
		say("param %-20s = %#llx (%#x)", params[i].name, p.value, kr);
		if (kr != KERN_SUCCESS) {
			failures++;
		}
	}

	struct create_bo bo = { .size = 16384 };
	kr = request(conn, 0x42, &bo, sizeof(bo));
	say("create_bo: handle %u, GPU address %#llx (%#x)", bo.handle, bo.offset, kr);
	if (kr != KERN_SUCCESS) {
		return 1;
	}

	struct mmap_bo mm = { .handle = bo.handle };
	kr = request(conn, 0x43, &mm, sizeof(mm));
	say("mmap_bo: offset %#llx (%#x)", mm.offset, kr);

	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	kr = IOConnectMapMemory64(conn, bo.handle, mach_task_self(), &addr, &len, SWEET_ANYWHERE);
	say("mapped: %#llx, %llu bytes (%#x)", (unsigned long long)addr, (unsigned long long)len, kr);
	if (kr != KERN_SUCCESS || addr == 0) {
		return 1;
	}

	volatile uint32_t * job = (volatile uint32_t *)(uintptr_t)addr;
	volatile uint32_t * target = (volatile uint32_t *)(uintptr_t)(addr + 0x800);
	uint64_t gpu = bo.offset;

	/* the mapping is uncached: word stores only (memset may use DC ZVA, which faults on device memory) */
	for (int i = 0; i < 0x1000 / 4; i++) {
		job[i] = 0;
	}
	*target = 0xdeadbeef;
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value, 64-bit pointers, job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee02u;

	uint32_t handles[1] = { bo.handle };
	struct submit sub = { .jc = gpu, .bo_handles = (uint64_t)(uintptr_t)handles, .bo_handle_count = 1 };
	kr = request(conn, 0x40, &sub, sizeof(sub));
	say("submit: %#x; word is %#x (wanted 0xc0ffee02)", kr, *target);
	if (kr != KERN_SUCCESS || *target != 0xc0ffee02u) {
		failures++;
	}

	/* a second chain: the buffer is reused, the word changes */
	*target = 0;
	job[12] = 0xc0ffee03u;
	kr = request(conn, 0x40, &sub, sizeof(sub));
	say("submit 2: %#x; word is %#x (wanted 0xc0ffee03)", kr, *target);
	if (kr != KERN_SUCCESS || *target != 0xc0ffee03u) {
		failures++;
	}

	struct gem_close gc = { .handle = bo.handle };
	kr = request(conn, 0x09, &gc, sizeof(gc));
	say("gem_close: %#x", kr);
	IOServiceClose(conn);
	IOObjectRelease(service);
	say(failures == 0 ? "RESULT: the GPU ran our jobs from a program" : "RESULT: %d step(s) failed", failures);
	return failures != 0;
}
