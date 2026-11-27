/*
 * libdrm for PanVK on iOS (see xf86drm.h).
 *
 * GPU requests: Linux's panfrost requests (0x40-0x4f) and GEM_CLOSE are calls of SweetMali's IOKit user client,
 * the request's number being the selector, its structure going in and coming back. Mesa knows the device by a
 * descriptor of /dev/null; there is one GPU and one connection to it in a process.
 *
 * Synchronisation objects, two ways:
 * - In the kernel (patch 0135, the job queue): the connection asks SweetMali for queued mode (request 0x4f). Then a
 *   SUBMIT only queues its chain and returns, the chain's out_sync is signalled by the kernel when the chain ends,
 *   and the syncobj requests go to SweetMali, waits included (they sleep there, with the service free for the
 *   process's other threads). Linux's waits take an absolute CLOCK_MONOTONIC time; the kernel takes a time from now,
 *   at most 10 s a call, so the waits here convert and go round again until the time is up.
 * - Here, in this process, when the kernel has no queue (an older kernel, boot-arg sweetmaliasync=0) or refuses, or
 *   with /var/sweet/mali-sync-off present or SWEET_MALI_SYNC=0: exact as long as SweetMali runs every job chain to
 *   its end before SUBMIT returns. So a syncobj is either signalled or has no fence at all, a SUBMIT signals its
 *   out_sync when it returns, and an unsignalled one can only become signalled by another thread of this process
 *   (a SUBMIT or a CPU signal), which a waiter with WAIT_FOR_SUBMIT waits for.
 * Sync files (PanVK moves fences between syncobjs through them) are descriptors of /dev/null standing for what they
 * carried: a state here, a kernel syncobj of their own there.
 * Not supported: opaque syncobj descriptors, dma-bufs, timelines (DRM_CAP_SYNCOBJ_TIMELINE is 0: Mesa's
 * vk_sync_timeline emulates them on binary syncobjs, as it does for PanVK on Bifrost anyway).
 *
 * The IOKit calls are declared here: the iOS SDK has no IOKitLib.h.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include "xf86drm.h"

/* OpenGL ES self-coherency (below, at the host imports) */
static int gl_self_coherent;
static void hosts_flush(const uint32_t *handles, uint32_t count);
static void host_forget(uint32_t handle);

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t, void *matching);
extern kern_return_t IOServiceOpen(io_service_t, task_t, uint32_t type, io_connect_t *);
extern kern_return_t IOObjectRelease(io_object_t);
extern kern_return_t IOConnectCallStructMethod(io_connect_t, uint32_t selector, const void *in, size_t inSize,
    void *out, size_t *outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t, uint32_t type, task_t, mach_vm_address_t *address,
    mach_vm_size_t *size, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t, uint32_t type, task_t, mach_vm_address_t address);
extern int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

#define SWEET_SERVICE           "SweetMali"
#define SWEET_ANYWHERE          1u
#define SWEET_RETURN_NO_MEMORY  0xe00002bdu
#define SWEET_RETURN_BAD_ARG    0xe00002c2u
#define SWEET_RETURN_TIMEOUT    0xe00002d6u
#define SWEET_RETURN_BUSY       0xe00002d5u
#define SWEET_RETURN_NOT_FOUND  0xe00002f0u
#define SWEET_RETURN_UNSUPPORTED 0xe00002c7u

#define PANFROST_SUBMIT_NR      0x40
#define PANFROST_WAIT_BO_NR     0x41
#define SWEET_MODE_NR           0x4f    /* ours (0135): queued SUBMITs and the kernel's syncobjs */
#define SYNCOBJ_CREATE_NR       0xbf
#define SYNCOBJ_DESTROY_NR      0xc0
#define SYNCOBJ_WAIT_NR         0xc3
#define SYNCOBJ_RESET_NR        0xc4
#define SYNCOBJ_SIGNAL_NR       0xc5
#define SYNCOBJ_TRANSFER_NR     0xcc

/* 1 once SweetMali took this process into queued mode: the syncobjs are the kernel's. */
static int kernel_sync;

/* SUBMITs made (job chains sent to the GPU): read by the Metal driver for its telemetry's per-commit chain counts. */
static atomic_ullong sweet_submits;
__attribute__((visibility("default"))) unsigned long long sweet_drm_submits(void);
__attribute__((visibility("default"))) unsigned long long
sweet_drm_submits(void)
{
	return atomic_load_explicit(&sweet_submits, memory_order_relaxed);
}

/*
 * PanVK's closed batches by call site (patch-panvk.py 3h): (caller, caller's caller) -> batches, with a fragment
 * job, with vertex/tiler jobs. sweet_drm_batch_sites writes up to `max` sites, busiest first, five words each:
 * the two return addresses as offsets in this image, then the three counts; it returns how many it wrote.
 */
#include <dlfcn.h>
#define BATCH_DEPTH 4
struct batch_site { uintptr_t a[BATCH_DEPTH]; unsigned long long n, frag, vtc; };
static struct batch_site batch_sites[64];
static pthread_mutex_t batch_lock = PTHREAD_MUTEX_INITIALIZER;
__attribute__((visibility("default"))) void sweet_drm_note_batch(void *a0, void *a1, int frag, int vtc);
__attribute__((visibility("default"))) __attribute__((noinline)) void
sweet_drm_note_batch(void *a0, void *a1, int frag, int vtc)
{
	uintptr_t a[BATCH_DEPTH] = { (uintptr_t)a0, (uintptr_t)a1 };
	/* two more callers up the frame-pointer chain (Darwin arm64 keeps it): fp -> [saved fp, return address] */
	uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
	for (int d = 0; d < 3 && fp != NULL && ((uintptr_t)fp & 7) == 0; d++)
		fp = (uintptr_t *)fp[0];            /* note_batch's -> close_batch's -> its caller's -> ... */
	for (int k = 2; k < BATCH_DEPTH && fp != NULL && ((uintptr_t)fp & 7) == 0; k++) {
		a[k] = fp[1];
		fp = (uintptr_t *)fp[0];
	}
	pthread_mutex_lock(&batch_lock);
	for (int i = 0; i < 64; i++) {
		struct batch_site *b = &batch_sites[i];
		if (b->n == 0 || memcmp(b->a, a, sizeof(a)) == 0) {
			memcpy(b->a, a, sizeof(a));
			b->n++;
			b->frag += frag != 0;
			b->vtc += vtc != 0;
			break;
		}
	}
	pthread_mutex_unlock(&batch_lock);
}
/* Up to `max` sites, busiest first, seven words each: four return addresses as offsets in this image, then counts. */
__attribute__((visibility("default"))) int sweet_drm_batch_sites(unsigned long long *out, int max);
__attribute__((visibility("default"))) int
sweet_drm_batch_sites(unsigned long long *out, int max)
{
	struct batch_site copy[64];
	Dl_info info;
	uintptr_t base = 0;
	int n = 0;

	if (dladdr((const void *)sweet_drm_batch_sites, &info) != 0)
		base = (uintptr_t)info.dli_fbase;
	pthread_mutex_lock(&batch_lock);
	memcpy(copy, batch_sites, sizeof(copy));
	pthread_mutex_unlock(&batch_lock);
	for (; n < max; n++) {
		int best = -1;
		for (int i = 0; i < 64; i++)
			if (copy[i].n != 0 && (best < 0 || copy[i].n > copy[best].n))
				best = i;
		if (best < 0)
			break;
		for (int k = 0; k < BATCH_DEPTH; k++)
			out[7 * n + k] = copy[best].a[k] >= base ? copy[best].a[k] - base : 0;
		out[7 * n + 4] = copy[best].n;
		out[7 * n + 5] = copy[best].frag;
		out[7 * n + 6] = copy[best].vtc;
		copy[best].n = 0;
	}
	return n;
}

/* A short trace of what Mesa asks of the GPU: lines on stderr and in /var/tmp/sweetdrm.log. */
static atomic_int trace_lines;
static void
trace(const char *fmt, ...)
{
	char line[200];
	va_list ap;
	int n;

	if (atomic_fetch_add(&trace_lines, 1) >= 600) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	(void)write(2, line, (size_t)n);
	int f = open("/var/tmp/sweetdrm.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (f >= 0) {
		(void)write(f, line, (size_t)n);
		close(f);
	}
}

static int
fail(int error)
{
	errno = error;
	return -1;
}

/* ---- the connection ---- */

static io_connect_t connection;
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static int call(unsigned number, void *data, size_t size);

/* Right after the connection opens, before any SUBMIT: queued mode, unless switched off. */
static void
choose_sync(void)
{
	struct { uint32_t mode, result; } m = { 1, 0 };
	const char *env = getenv("SWEET_MALI_SYNC");
	struct stat st;
	int error;

	if (env != NULL && strcmp(env, "0") == 0) {
		trace("sweetdrm: syncobjs in this process (SWEET_MALI_SYNC=0)");
		return;
	}
	if (stat("/var/sweet/mali-sync-off", &st) == 0) {
		trace("sweetdrm: syncobjs in this process (/var/sweet/mali-sync-off)");
		return;
	}
	/*
	 * Mode bit 2 (kernel patch 0151): this process cleans and invalidates the host pages it imported itself, so the
	 * kernel stops cleaning every imported page around every SUBMIT and wait. Only the Metal driver does that
	 * (it sets SWEET_MALI_SELF_COHERENT=1, its frontend flushes each command's host ranges by address); a kernel
	 * without 0151 refuses mode 3, and the request is repeated with mode 1.
	 */
	const char *self = getenv("SWEET_MALI_SELF_COHERENT");
	const char *gl = getenv("SWEET_GL_SELF_COHERENT");
	if ((self != NULL && strcmp(self, "1") == 0) || (gl != NULL && strcmp(gl, "1") == 0)) {
		m.mode = 3;
		error = call(SWEET_MODE_NR, &m, sizeof(m));
		if (error != 0 || (m.result & 1) == 0) {
			trace("sweetdrm: mode 3 refused (err %d, result %u): asking for mode 1", error, m.result);
			m.mode = 1;
			m.result = 0;
			error = call(SWEET_MODE_NR, &m, sizeof(m));
		}
	} else {
		error = call(SWEET_MODE_NR, &m, sizeof(m));
	}
	kernel_sync = error == 0 && (m.result & 1) != 0;
	gl_self_coherent = (m.result & 2) != 0 && gl != NULL && strcmp(gl, "1") == 0;
	if (gl_self_coherent) {
		trace("sweetdrm: OpenGL ES: this process cleans its host imports before each SUBMIT");
	}
	trace("sweetdrm: %s%s (mode request %u -> err %d, result %u)", kernel_sync ? "queued SUBMITs, the kernel's syncobjs" :
	    "every chain run to its end in SUBMIT, syncobjs in this process",
	    (m.result & 2) != 0 ? ", imported pages kept coherent by this process" : "", m.mode, error, m.result);
}

static int
connect_service(void)
{
	pthread_mutex_lock(&open_lock);
	if (connection == 0) {
		io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(SWEET_SERVICE));
		if (service != 0) {
			kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
			if (kr != 0) {
				connection = 0;
			}
			trace("sweetdrm: open %s -> %#x", SWEET_SERVICE, kr);
			IOObjectRelease(service);
			if (connection != 0) {
				choose_sync();
			}
		} else {
			trace("sweetdrm: no %s service", SWEET_SERVICE);
		}
	}
	pthread_mutex_unlock(&open_lock);
	return connection != 0 ? 0 : -1;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if ((name != NULL && strcmp(name, "panfrost") != 0) || connect_service() != 0) {
		return -1;
	}
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (connection == 0 && connect_service() != 0) {
		return ENODEV;
	}
	result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	switch (result) {
	case 0:
		return 0;
	case SWEET_RETURN_NO_MEMORY:
		return ENOMEM;
	case SWEET_RETURN_BAD_ARG:
		return EINVAL;
	case SWEET_RETURN_TIMEOUT:
		return ETIMEDOUT;
	case SWEET_RETURN_BUSY:
		return EBUSY;
	case SWEET_RETURN_NOT_FOUND:
		return ENOENT;
	case SWEET_RETURN_UNSUPPORTED:
		return ENOSYS;
	default:
		return EIO;
	}
}

/* connect_service() once, for the syncobj calls, which may come before any request: then kernel_sync is known. */
static void
connected(void)
{
	if (connection == 0) {
		(void)connect_service();
	}
}

/* ---- synchronisation objects ---- */

#define SYNCOBJ_MAX 4096
static unsigned char sync_used[SYNCOBJ_MAX + 1], sync_signalled[SYNCOBJ_MAX + 1];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_changed = PTHREAD_COND_INITIALIZER;

#define SYNC_FILES 256
static struct { int fd; unsigned char signalled; uint32_t kernel; } sync_files[SYNC_FILES];

/* ---- the kernel's syncobjs (queued mode) ---- */

struct k_wait {                         /* SweetMali's SYNCOBJ_WAIT: Linux's, with a time to wait from now */
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};

static int
k_create(uint32_t flags, uint32_t *handle)
{
	struct { uint32_t handle, flags; } c = { 0, flags };
	int error = call(SYNCOBJ_CREATE_NR, &c, sizeof(c));

	if (error != 0) {
		trace("sweetdrm: kernel SYNCOBJ_CREATE -> err %d", error);
		return fail(error);
	}
	*handle = c.handle;
	return 0;
}

static int
k_destroy(uint32_t handle)
{
	struct { uint32_t handle, pad; } d = { handle, 0 };
	int error = call(SYNCOBJ_DESTROY_NR, &d, sizeof(d));
	return error == 0 ? 0 : fail(error);
}

static int
k_set(const uint32_t *handles, uint32_t count, int signal)
{
	struct { uint64_t handles; uint32_t count, pad; } a = { (uint64_t)(uintptr_t)handles, count, 0 };
	int error = call(signal ? SYNCOBJ_SIGNAL_NR : SYNCOBJ_RESET_NR, &a, sizeof(a));
	return error == 0 ? 0 : fail(error);
}

static int
k_transfer(uint32_t dst, uint32_t src)
{
	struct { uint32_t src, dst; uint64_t src_point, dst_point; uint32_t flags, pad; } t = { src, dst, 0, 0, 0, 0 };
	int error = call(SYNCOBJ_TRANSFER_NR, &t, sizeof(t));
	return error == 0 ? 0 : fail(error);
}

static uint64_t now_ns(void);

/* A Linux deadline (absolute CLOCK_MONOTONIC ns; INT64_MAX: none) as a time from now, for SweetMali. */
static int64_t
time_left(int64_t deadline)
{
	uint64_t t = now_ns();

	if (deadline == INT64_MAX) {
		return INT64_MAX;
	}
	return deadline > 0 && (uint64_t)deadline > t ? (int64_t)((uint64_t)deadline - t) : 0;
}

/* libdrm's convention: 0, or -errno (ETIME when the time ran out). */
static int
k_wait(const uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
	for (;;) {
		struct k_wait w = { (uint64_t)(uintptr_t)handles, time_left(timeout_nsec), count, flags, 0, 0, 0 };
		int error = call(SYNCOBJ_WAIT_NR, &w, sizeof(w));

		if (error == 0) {
			if (first != NULL) {
				*first = w.first_signaled;
			}
			return 0;
		}
		if (error == ETIMEDOUT) {
			/* SweetMali waits at most 10 s a call: again, until the caller's time is up */
			if (timeout_nsec != INT64_MAX && now_ns() >= (uint64_t)(timeout_nsec > 0 ? timeout_nsec : 0)) {
				errno = ETIME;
				return -ETIME;
			}
			continue;
		}
		errno = error;
		return -error;
	}
}

static int
valid(uint32_t h)
{
	return h != 0 && h <= SYNCOBJ_MAX && sync_used[h];
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	connected();
	if (kernel_sync) {
		return k_create(flags, handle);
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t h = 1; h <= SYNCOBJ_MAX; h++) {
		if (!sync_used[h]) {
			sync_used[h] = 1;
			sync_signalled[h] = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
			pthread_mutex_unlock(&sync_lock);
			*handle = h;
			return 0;
		}
	}
	pthread_mutex_unlock(&sync_lock);
	trace("sweetdrm: out of syncobjs");
	return fail(ENOMEM);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	int ok;

	(void)fd;
	if (kernel_sync) {
		return k_destroy(handle);
	}
	pthread_mutex_lock(&sync_lock);
	ok = valid(handle);
	if (ok) {
		sync_used[handle] = 0;
		sync_signalled[handle] = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	return ok ? 0 : fail(EINVAL);
}

static int
set_state(const uint32_t *handles, uint32_t count, unsigned char state)
{
	if (kernel_sync) {
		return count == 0 ? 0 : k_set(handles, count, state);
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		if (!valid(handles[i])) {
			pthread_mutex_unlock(&sync_lock);
			return fail(EINVAL);
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		sync_signalled[handles[i]] = state;
	}
	if (state) {
		pthread_cond_broadcast(&sync_changed);
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return set_state(handles, count, 0);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return set_state(handles, count, 1);
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (points != NULL && points[i] != 0) {
			return fail(EINVAL);
		}
	}
	return drmSyncobjSignal(fd, handles, count);
}

static uint64_t
now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

/* libdrm's convention for the waits: 0, or -errno (ETIME when the time ran out). */
static int
wait_syncobjs(const uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
	int all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	int for_submit = (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) != 0;
	int result;

	if (count == 0) {
		errno = EINVAL;
		return -EINVAL;
	}
	if (kernel_sync) {
		return k_wait(handles, count, timeout_nsec, flags, first);
	}
	pthread_mutex_lock(&sync_lock);
	for (;;) {
		unsigned done = 0, firstdone = count;

		for (unsigned i = 0; i < count; i++) {
			if (!valid(handles[i])) {
				pthread_mutex_unlock(&sync_lock);
				errno = EINVAL;
				return -EINVAL;
			}
			if (sync_signalled[handles[i]]) {
				done++;
				if (firstdone == count) {
					firstdone = i;
				}
			}
		}
		if (all ? done == count : done > 0) {
			if (first != NULL) {
				*first = firstdone == count ? 0 : firstdone;
			}
			result = 0;
			break;
		}
		/* No fence: without WAIT_FOR_SUBMIT Linux says EINVAL; with it, wait for this process to signal. */
		if (!for_submit) {
			result = -EINVAL;
			break;
		}
		uint64_t t = now_ns();
		if (timeout_nsec <= 0 || (uint64_t)timeout_nsec <= t) {
			result = -ETIME;
			break;
		}
		uint64_t left = (uint64_t)timeout_nsec - t;
		if (left > 3600ull * 1000000000ull) {
			left = 3600ull * 1000000000ull;
		}
		struct timespec rel = { .tv_sec = (time_t)(left / 1000000000ull), .tv_nsec = (long)(left % 1000000000ull) };
		pthread_cond_timedwait_relative_np(&sync_changed, &sync_lock, &rel);
	}
	pthread_mutex_unlock(&sync_lock);
	if (result != 0) {
		errno = -result;
	}
	return result;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	return wait_syncobjs(handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	for (unsigned i = 0; i < num_handles; i++) {
		if (points != NULL && points[i] != 0) {
			errno = EINVAL;
			return -EINVAL;
		}
	}
	return wait_syncobjs(handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (kernel_sync) {
		/* 1 for a signalled one: a wait that does not wait (WAIT_FOR_SUBMIT: one without a chain is not an error) */
		for (uint32_t i = 0; i < handle_count; i++) {
			struct k_wait w = { (uint64_t)(uintptr_t)&handles[i], 0, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0, 0, 0 };
			int error = call(SYNCOBJ_WAIT_NR, &w, sizeof(w));

			if (error != 0 && error != ETIMEDOUT) {
				return fail(error);
			}
			points[i] = error == 0;
		}
		return 0;
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < handle_count; i++) {
		if (!valid(handles[i])) {
			pthread_mutex_unlock(&sync_lock);
			return fail(EINVAL);
		}
		points[i] = sync_signalled[handles[i]];
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (dst_point != 0 || src_point != 0) {
		return fail(EINVAL);
	}
	if (kernel_sync) {
		return k_transfer(dst_handle, src_handle);
	}
	pthread_mutex_lock(&sync_lock);
	if (!valid(dst_handle) || !valid(src_handle)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	sync_signalled[dst_handle] = sync_signalled[src_handle];
	pthread_cond_broadcast(&sync_changed);
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	uint32_t carried = 0;

	(void)fd;
	if (kernel_sync) {
		/* the sync file's own kernel syncobj, holding what `handle` holds now */
		if (k_create(0, &carried) != 0) {
			return -1;
		}
		if (k_transfer(carried, handle) != 0) {
			int error = errno;
			(void)k_destroy(carried);
			return fail(error);
		}
	}
	pthread_mutex_lock(&sync_lock);
	if (!kernel_sync && !valid(handle)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	int f = open("/dev/null", O_RDONLY | O_CLOEXEC), slot = -1;
	if (f < 0) {
		pthread_mutex_unlock(&sync_lock);
		if (carried != 0) {
			(void)k_destroy(carried);
		}
		return -1;
	}
	/* an entry with this number is stale (its descriptor was closed without an import): replace it */
	for (int i = 0; i < SYNC_FILES && slot < 0; i++) {
		if (sync_files[i].fd == f) {
			slot = i;
			if (sync_files[i].kernel != 0) {
				(void)k_destroy(sync_files[i].kernel);
				sync_files[i].kernel = 0;
			}
		}
	}
	for (int i = 0; i < SYNC_FILES && slot < 0; i++) {
		if (sync_files[i].fd <= 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&sync_lock);
		close(f);
		if (carried != 0) {
			(void)k_destroy(carried);
		}
		trace("sweetdrm: out of sync files");
		return fail(ENOMEM);
	}
	sync_files[slot].fd = f;
	sync_files[slot].signalled = kernel_sync ? 0 : sync_signalled[handle];
	sync_files[slot].kernel = carried;
	pthread_mutex_unlock(&sync_lock);
	*sync_file_fd = f;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	unsigned char state = 1;        /* -1: "already signalled" */
	uint32_t carried = 0;

	(void)fd;
	pthread_mutex_lock(&sync_lock);
	if (!kernel_sync && !valid(handle)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	if (sync_file_fd >= 0) {
		int found = 0;
		for (int i = 0; i < SYNC_FILES; i++) {
			if (sync_files[i].fd == sync_file_fd) {
				/* one import per export (PanVK closes the descriptor right after) */
				state = sync_files[i].signalled;
				carried = sync_files[i].kernel;
				sync_files[i].fd = 0;
				sync_files[i].kernel = 0;
				found = 1;
				break;
			}
		}
		if (!found) {
			pthread_mutex_unlock(&sync_lock);
			return fail(EINVAL);
		}
	}
	if (kernel_sync) {
		int r;

		pthread_mutex_unlock(&sync_lock);
		if (carried == 0) {
			return k_set(&handle, 1, 1);    /* -1, or a sync file from before queued mode: signalled */
		}
		r = k_transfer(handle, carried);
		int error = errno;
		(void)k_destroy(carried);
		return r == 0 ? 0 : fail(error);
	}
	sync_signalled[handle] = state;
	pthread_cond_broadcast(&sync_changed);
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd; (void)obj_fd; (void)handle;
	return fail(ENOSYS);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd; (void)handle; (void)obj_fd;
	return fail(ENOSYS);
}

/* ---- requests ---- */

static int
local_ioctl(unsigned long request, void *arg)
{
	switch (request) {
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap *c = arg;
		return drmGetCap(-1, c->capability, &c->value);
	}
	case DRM_IOCTL_SYNCOBJ_CREATE: {
		struct drm_syncobj_create *c = arg;
		return drmSyncobjCreate(-1, c->flags, &c->handle);
	}
	case DRM_IOCTL_SYNCOBJ_DESTROY: {
		struct drm_syncobj_destroy *d = arg;
		return drmSyncobjDestroy(-1, d->handle);
	}
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: {
		struct drm_syncobj_handle *h = arg;
		if (!(h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
			return fail(ENOSYS);
		}
		return drmSyncobjExportSyncFile(-1, h->handle, &h->fd);
	}
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: {
		struct drm_syncobj_handle *h = arg;
		if (!(h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) {
			return fail(ENOSYS);
		}
		return drmSyncobjImportSyncFile(-1, h->handle, h->fd);
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait *w = arg;
		int r = wait_syncobjs((const uint32_t *)(uintptr_t)w->handles, w->count_handles, w->timeout_nsec,
		    w->flags, &w->first_signaled);
		return r == 0 ? 0 : fail(-r);
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
		struct drm_syncobj_timeline_wait *w = arg;
		int r = drmSyncobjTimelineWait(-1, (uint32_t *)(uintptr_t)w->handles, (uint64_t *)(uintptr_t)w->points,
		    w->count_handles, w->timeout_nsec, w->flags, &w->first_signaled);
		return r == 0 ? 0 : fail(-r);
	}
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array *a = arg;
		return set_state((const uint32_t *)(uintptr_t)a->handles, a->count_handles,
		    request == DRM_IOCTL_SYNCOBJ_SIGNAL);
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: {
		struct drm_syncobj_timeline_array *a = arg;
		return drmSyncobjTimelineSignal(-1, (const uint32_t *)(uintptr_t)a->handles,
		    (uint64_t *)(uintptr_t)a->points, a->count_handles);
	}
	case DRM_IOCTL_SYNCOBJ_QUERY: {
		struct drm_syncobj_timeline_array *a = arg;
		return drmSyncobjQuery2(-1, (uint32_t *)(uintptr_t)a->handles, (uint64_t *)(uintptr_t)a->points,
		    a->count_handles, a->flags);
	}
	case DRM_IOCTL_SYNCOBJ_TRANSFER: {
		struct drm_syncobj_transfer *t = arg;
		return drmSyncobjTransfer(-1, t->dst_handle, t->dst_point, t->src_handle, t->src_point, t->flags);
	}
	default:
		trace("sweetdrm: unsupported request %#lx", request);
		return fail(ENOSYS);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	uint32_t before[6] = { 0 };
	int error;

	(void)fd;
	if (number != 0x09 && (number < 0x40 || number > 0x4f)) {
		return local_ioctl(request, arg);
	}
	if (number == 0x42) {
		memcpy(before, arg, IOCPARM_LEN(request) < sizeof(before) ? IOCPARM_LEN(request) : sizeof(before));
	}
	/*
	 * MADVISE (struct drm_panfrost_madvise: u32 handle, madv, retained): Mesa marks every buffer it puts into its cache
	 * DONTNEED and every one it takes out WILLNEED, two kernel calls per reuse. SweetMali never takes a buffer's pages
	 * back, so a buffer is always retained: answered here (2026-11-26, after 0157 made the cache hit).
	 */
	if (number == 0x48 && IOCPARM_LEN(request) >= 12) {
		((uint32_t *)arg)[2] = 1;
		return 0;
	}
	if (gl_self_coherent && number == PANFROST_SUBMIT_NR) {
		/* struct drm_panfrost_submit: u64 bo_handles at 24, u32 bo_handle_count at 32 (fix15) */
		const uint8_t *submit = arg;
		uint64_t list;
		uint32_t count;

		memcpy(&list, submit + 24, sizeof(list));
		memcpy(&count, submit + 32, sizeof(count));
		hosts_flush((const uint32_t *)(uintptr_t)list, count);
	}
	if (gl_self_coherent && number == 0x09) {
		host_forget(*(const uint32_t *)arg);
	}
	if (number == PANFROST_WAIT_BO_NR && kernel_sync) {
		/* struct drm_panfrost_wait_bo: u32 handle, pad; s64 timeout_ns, absolute -- SweetMali takes a time from now */
		struct { uint32_t handle, pad; int64_t timeout_ns; } *w = arg, k;
		int64_t deadline = w->timeout_ns;

		for (;;) {
			k = *w;
			k.timeout_ns = time_left(deadline);
			error = call(number, &k, sizeof(k));
			if (error != ETIMEDOUT || deadline == INT64_MAX || now_ns() >= (uint64_t)(deadline > 0 ? deadline : 0)) {
				break;
			}
		}
		if (error == ETIMEDOUT && k.timeout_ns == 0) {
			error = EBUSY;                  /* Linux: EBUSY for a look that does not wait */
		}
		return error == 0 ? 0 : fail(error);
	}
	error = call(number, arg, IOCPARM_LEN(request));
	if (number == 0x42) {
		const uint32_t *r = arg;
		trace("sweetdrm: CREATE_BO size %#x flags %#x -> err %d, handle %u gpu %#x%08x", before[0], before[1],
		    error, r[2], r[5], r[4]);
	} else if (error != 0) {
		trace("sweetdrm: request %#x (%lu bytes) -> err %d", number, (unsigned long)IOCPARM_LEN(request), error);
	}
	if (error != 0) {
		return fail(error);
	}
	/*
	 * Without queued mode the job chain has run to its end: its out_sync is signalled here (struct
	 * drm_panfrost_submit: u32 at 20). In queued mode the kernel signals it when the chain ends.
	 */
	if (number == PANFROST_SUBMIT_NR) {
		/* chains sent, for the Metal driver's per-commit telemetry (sweet_drm_submits, looked up with dlsym) */
		atomic_fetch_add_explicit(&sweet_submits, 1, memory_order_relaxed);
	}
	if (number == PANFROST_SUBMIT_NR && !kernel_sync) {
		uint32_t out = ((const uint32_t *)arg)[5];
		if (out != 0) {
			(void)set_state(&out, 1, 1);
		}
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* panfrost 1.3: timestamps; no WB mappings, JM contexts or SYNC_BO (1.5/1.6), which SweetMali has not got. */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 3;
		version->name = strdup("panfrost");
		version->name_len = 8;
		version->date = strdup("");
		version->desc = strdup("Mali-G52 on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	*value = capability == DRM_CAP_SYNCOBJ ? 1 : 0;
	return 0;
}

int
drmGetMagic(int fd, drm_magic_t *magic)
{
	(void)fd;
	(void)magic;
	return fail(EINVAL);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close_request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_request);
}

/* ---- the device ---- */

#define SWEET_NODE "/dev/null"

struct sweet_device {
	drmDevice device;               /* first: freeing it frees all of this */
	char *nodes[DRM_NODE_MAX];
	char render[16];
	drmPlatformBusInfo bus;
	drmPlatformDeviceInfo info;
	char *compatible[2];
	char compatible0[32];
};

static drmDevicePtr
make_device(void)
{
	struct sweet_device *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		return NULL;
	}
	strcpy(s->render, SWEET_NODE);
	s->nodes[DRM_NODE_RENDER] = s->render;
	strcpy(s->bus.fullname, "/sweetmali");
	strcpy(s->compatible0, "arm,mali-bifrost");
	s->compatible[0] = s->compatible0;
	s->info.compatible = s->compatible;
	s->device.nodes = s->nodes;
	s->device.available_nodes = 1 << DRM_NODE_RENDER;
	s->device.bustype = DRM_BUS_PLATFORM;
	s->device.businfo.platform = &s->bus;
	s->device.deviceinfo.platform = &s->info;
	return &s->device;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (connect_service() != 0) {
		return 0;
	}
	if (devices == NULL) {
		return 1;
	}
	if (max_devices < 1) {
		return 0;
	}
	devices[0] = make_device();
	return devices[0] != NULL ? 1 : -ENOMEM;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (connect_service() != 0) {
		return -ENODEV;
	}
	*device = make_device();
	return *device != NULL ? 0 : -ENOMEM;
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)dev_id;
	return drmGetDevice2(-1, flags, device);
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		free(*device);
		*device = NULL;
	}
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype && a->bustype == DRM_BUS_PLATFORM &&
	    strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname) == 0;
}

char *
drmGetDeviceNameFromFd2(int fd)
{
	(void)fd;
	return strdup(SWEET_NODE);
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
	(void)fd;
	return strdup(SWEET_NODE);
}

int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

/*
 * Host-pointer imports (VK_EXT_external_memory_host, patched into PanVK): SweetMali's IMPORT_HOST (kernel patch 0128)
 * wires the caller's pages and makes them a buffer. PanVK then takes the buffer through its dma-buf import
 * (pan_kmod_bo_import(fd)), so the buffer travels as a descriptor of /dev/null that stands for its handle and size:
 * drmPrimeFDToHandle and sweet_drm_fd_size answer for it, sweet_drm_forget_fd drops it.
 */
#define SWEET_IMPORT_HOST_NR 0x4e
#define IMPORTS 256

/*
 * 2026-11-26, the OpenGL ES library in the window server (libsweet-gl, Mesa Gallium Panfrost over this shim): mode 3
 * for it too. Round z's profile of backboardd on OpenGL ES: 60% of its time in the kernel, most of that cleaning
 * every imported page around every SUBMIT (clean_mmu_dcache, FlushPoC, CleanPoC) -- the cost 0151 took off the Metal
 * driver. With SWEET_GL_SELF_COHERENT=1 (set by libsweet-gl) this process does it instead, by address (DC CIVAC
 * reaches every core's caches): before a SUBMIT, every live host import is cleaned and invalidated -- the small ones
 * (glyph masks, icons, < 1 MB) at every SUBMIT, the big ones (window-sized surfaces, the frame's target) once a frame
 * (sweet_drm_frame_mark(), called by libsweet-gl when a frame has been handed on).
 */
#include <libkern/OSCacheControl.h>
#define HOSTS 512
#define HOST_BIG (1u << 20)
static struct { uint32_t handle; void *address; uint64_t size; unsigned gen; int gpu_only; } hosts[HOSTS];
/*
 * fix14: memory only the GPU writes and reads (the display's surfaces and CoreAnimation's offscreen ones, named by
 * libsweet-gl, sweet_drm_host_gpu_only()) is not cleaned: no processor writes it, so nothing of it is dirty in a cache.
 * fix13's profile: a quarter of CoreAnimation's render thread went to cleaning (DC CIVAC) window-sized surfaces once a
 * frame, the two display surfaces among them.
 */
#define GPU_ONLY 64
static void *gpu_only_at[GPU_ONLY];
static unsigned gpu_only_count;
static unsigned host_gen = 1, host_count;
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t host_flushed_bytes, host_flushes;

__attribute__((visibility("default"))) void sweet_drm_frame_mark(void);
__attribute__((visibility("default"))) void
sweet_drm_frame_mark(void)
{
	pthread_mutex_lock(&host_lock);
	host_gen++;
	pthread_mutex_unlock(&host_lock);
}

/* bytes cleaned by this process before SUBMITs, and how many SUBMITs (for libsweet-gl's frame lines) */
__attribute__((visibility("default"))) void sweet_drm_host_flush_stats(unsigned long long *bytes, unsigned long long *submits);
__attribute__((visibility("default"))) void
sweet_drm_host_flush_stats(unsigned long long *bytes, unsigned long long *submits)
{
	pthread_mutex_lock(&host_lock);
	*bytes = host_flushed_bytes;
	*submits = host_flushes;
	pthread_mutex_unlock(&host_lock);
}

__attribute__((visibility("default"))) void sweet_drm_host_gpu_only(void *address);
__attribute__((visibility("default"))) void
sweet_drm_host_gpu_only(void *address)
{
	unsigned i;

	if (address == NULL) {
		return;
	}
	pthread_mutex_lock(&host_lock);
	for (i = 0; i < gpu_only_count && gpu_only_at[i] != address; i++) {
	}
	if (i == gpu_only_count && gpu_only_count < GPU_ONLY) {
		gpu_only_at[gpu_only_count++] = address;
	}
	for (unsigned j = 0, seen = 0; j < HOSTS && seen < host_count; j++) {
		if (hosts[j].address != NULL) {
			seen++;
			if (hosts[j].address == address && !hosts[j].gpu_only) {
				/*
				 * fix19: cleaned once, now. A new surface is zeroed by a processor before CoreAnimation draws into
				 * it: dirty lines of those zeros stayed in the caches (fix14 stopped cleaning such surfaces) and
				 * were written back at any later time over what the GPU had drawn -- flashing squares, icons
				 * repeated along their way, a small home screen in a corner (user, 2026-11-27 22:46).
				 */
				sys_dcache_flush(hosts[j].address, (size_t)hosts[j].size);
				host_flushed_bytes += hosts[j].size;
				hosts[j].gpu_only = 1;
			}
		}
	}
	pthread_mutex_unlock(&host_lock);
}

static void
host_note(uint32_t handle, void *address, uint64_t size)
{
	pthread_mutex_lock(&host_lock);
	for (unsigned i = 0; i < HOSTS; i++) {
		if (hosts[i].address == NULL) {
			hosts[i].handle = handle;
			hosts[i].address = address;
			hosts[i].size = size;
			hosts[i].gen = 0;
			hosts[i].gpu_only = 0;
			for (unsigned k = 0; k < gpu_only_count; k++) {
				hosts[i].gpu_only |= gpu_only_at[k] == address;
			}
			if (hosts[i].gpu_only) {
				sys_dcache_flush(address, (size_t)size);        /* (fix19: once, as it comes) */
				host_flushed_bytes += size;
			}
			host_count++;
			break;
		}
	}
	pthread_mutex_unlock(&host_lock);
}

static void
host_forget(uint32_t handle)
{
	pthread_mutex_lock(&host_lock);
	for (unsigned i = 0; i < HOSTS && host_count != 0; i++) {
		if (hosts[i].address != NULL && hosts[i].handle == handle) {
			/* (memory that goes may come back as somebody else's, written by a processor: not taken for GPU-only) */
			for (unsigned k = 0; hosts[i].gpu_only && k < gpu_only_count; k++) {
				if (gpu_only_at[k] == hosts[i].address) {
					gpu_only_at[k] = gpu_only_at[--gpu_only_count];
					break;
				}
			}
			hosts[i].address = NULL;
			host_count--;
			break;
		}
	}
	pthread_mutex_unlock(&host_lock);
}

/*
 * fix15: fix14's frame lines said 4 GB cleaned every 20 frames (about 200 MB a frame, ~7 MB at each of ~28 SUBMITs):
 * every small import was cleaned at every SUBMIT, used by it or not. Now only the imports the SUBMIT names in its BO
 * list (what the job reads or writes: Panfrost lists every BO of the batch) are cleaned, and each once a frame
 * (sweet_drm_frame_mark()), whatever its size. A processor write into a picture after its first use in a frame
 * shows from the next frame. With no list given (count 0), every import is taken as named, as before.
 */
static uint8_t host_named[65536 / 8];

static void
hosts_flush(const uint32_t *handles, uint32_t count)
{
	pthread_mutex_lock(&host_lock);
	host_flushes++;
	if (handles != NULL && count != 0) {
		for (uint32_t k = 0; k < count; k++) {
			host_named[(handles[k] & 0xffff) >> 3] |= (uint8_t)(1u << (handles[k] & 7));
		}
	}
	for (unsigned i = 0, seen = 0; i < HOSTS && seen < host_count; i++) {
		if (hosts[i].address == NULL) {
			continue;
		}
		seen++;
		if (hosts[i].gpu_only || hosts[i].gen == host_gen) {
			continue;
		}
		if (handles != NULL && count != 0 &&
		    (host_named[(hosts[i].handle & 0xffff) >> 3] & (1u << (hosts[i].handle & 7))) == 0) {
			continue;
		}
		/* the same range imported again (a surface made a texture every frame): once */
		int again = 0;
		for (unsigned j = 0; j < i && hosts[i].size >= HOST_BIG; j++) {
			if (hosts[j].address == hosts[i].address && hosts[j].gen == host_gen) {
				again = 1;
				break;
			}
		}
		hosts[i].gen = host_gen;
		if (!again) {
			sys_dcache_flush(hosts[i].address, (size_t)hosts[i].size);
			host_flushed_bytes += hosts[i].size;
		}
	}
	if (handles != NULL && count != 0) {
		for (uint32_t k = 0; k < count; k++) {
			host_named[(handles[k] & 0xffff) >> 3] = 0;
		}
	}
	pthread_mutex_unlock(&host_lock);
}
static struct { int fd; uint32_t handle; uint64_t size; } imports[IMPORTS];
static pthread_mutex_t import_lock = PTHREAD_MUTEX_INITIALIZER;

int
sweet_drm_import_host(void *address, uint64_t size)
{
	struct { uint64_t address, size; uint32_t handle, pad; uint64_t offset; } r = { (uint64_t)(uintptr_t)address, size, 0, 0, 0 };
	int error = call(SWEET_IMPORT_HOST_NR, &r, sizeof(r));
	trace("sweetdrm: IMPORT_HOST %p size %#llx -> err %d, handle %u gpu %#llx", address, (unsigned long long)size, error,
	    r.handle, (unsigned long long)r.offset);
	if (error != 0) {
		errno = error;
		return -1;
	}
	if (gl_self_coherent) {
		host_note(r.handle, address, size);
	}
	int f = open("/dev/null", O_RDONLY | O_CLOEXEC);
	pthread_mutex_lock(&import_lock);
	for (int i = 0; f >= 0 && i < IMPORTS; i++) {
		if (imports[i].fd <= 0) {
			imports[i].fd = f;
			imports[i].handle = r.handle;
			imports[i].size = size;
			pthread_mutex_unlock(&import_lock);
			return f;
		}
	}
	pthread_mutex_unlock(&import_lock);
	if (f >= 0) {
		close(f);
	}
	struct drm_gem_close c = { .handle = r.handle };
	(void)call(0x09, &c, sizeof(c));
	errno = ENOMEM;
	return -1;
}

uint64_t
sweet_drm_fd_size(int fd)
{
	uint64_t size = 0;
	pthread_mutex_lock(&import_lock);
	for (int i = 0; i < IMPORTS; i++) {
		if (imports[i].fd == fd && fd > 0) {
			size = imports[i].size;
			break;
		}
	}
	pthread_mutex_unlock(&import_lock);
	return size;
}

void
sweet_drm_forget_fd(int fd)
{
	pthread_mutex_lock(&import_lock);
	for (int i = 0; i < IMPORTS; i++) {
		if (imports[i].fd == fd && fd > 0) {
			imports[i].fd = 0;
			break;
		}
	}
	pthread_mutex_unlock(&import_lock);
	close(fd);
}

/* Buffers as file descriptors, for other programs: not here. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	return fail(ENOSYS);
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&import_lock);
	for (int i = 0; i < IMPORTS; i++) {
		if (imports[i].fd == prime_fd && prime_fd > 0) {
			*handle = imports[i].handle;
			pthread_mutex_unlock(&import_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&import_lock);
	return fail(ENOSYS);
}

/* ---- buffer mappings ---- */

#define MAP_TABLE 16384
static struct { uint64_t address; uint32_t type; } map_table[MAP_TABLE];
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

/* Is fd one of the device's descriptors, i.e. one of /dev/null? */
static int
device_fd(int fd)
{
	static dev_t null_rdev;
	static int known;
	struct stat st;

	if (!known) {
		if (stat(SWEET_NODE, &st) != 0) {
			return 0;
		}
		null_rdev = st.st_rdev;
		known = 1;
	}
	return fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == null_rdev;
}

/*
 * How buffers are mapped: write-combined, so that what this process writes reaches memory where the GPU reads it
 * with no cache maintenance; PanVK takes its memory to be host-coherent. The cache mode is the caller's to choose:
 * IOUserClient::mapClientMemory64 keeps the kIOMapUserOptionsMask bits (the cache mode among them) of these flags
 * over what clientMemoryForType answered, so the kernel's choice (patch 0127) alone did nothing (boot 2026-11-14:
 * the clear still faulted). boot-arg sweetmaliwc=0 (payload-wc0) keeps the cached mappings.
 */
#define SWEET_WRITE_COMBINE 0x400u      /* kIOMapWriteCombineCache = kIOWriteCombineCache (4) << kIOMapCacheShift (8) */

static uint32_t
map_options(void)
{
	static int wc = -1;

	if (wc < 0) {
		char args[1024] = "";
		size_t len = sizeof(args) - 1;
		wc = !(sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0 && strstr(args, "sweetmaliwc=0") != NULL);
		trace("sweetdrm: buffers are mapped %s", wc ? "write-combined" : "cached (sweetmaliwc=0)");
	}
	return SWEET_ANYWHERE | (wc ? SWEET_WRITE_COMBINE : 0);
}

/* MMAP_BO answered offset = handle << 12: the buffer is the service's client memory of type `handle`. */
static void *
map_buffer(size_t size, uint64_t offset)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;
	uint32_t type = (uint32_t)(offset >> 12);
	kern_return_t kr;

	if (connection == 0) {
		return MAP_FAILED;
	}
	kr = IOConnectMapMemory64(connection, type, mach_task_self(), &address, &mapped, map_options());
	if (kr != 0 || mapped < size) {
		trace("sweetdrm: MMAP handle %u size %#zx -> %#x (mapped %#llx)", type, size, kr,
		    (unsigned long long)mapped);
		if (kr == 0) {
			IOConnectUnmapMemory64(connection, type, mach_task_self(), address);
		}
		errno = ENOMEM;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&map_lock);
	for (int i = 0; i < MAP_TABLE; i++) {
		if (map_table[i].address == 0) {
			map_table[i].address = address;
			map_table[i].type = type;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
	return (void *)(uintptr_t)address;
}

void *
sweet_os_mmap(void *address, size_t size, int prot, int flags, int fd, long long offset)
{
	if (!(flags & MAP_ANON) && device_fd(fd)) {
		if (address != NULL && (flags & MAP_FIXED)) {
			trace("sweetdrm: placed mapping at %p refused", address);
			errno = EINVAL;
			return MAP_FAILED;
		}
		return map_buffer(size, (uint64_t)offset);
	}
	return mmap(address, size, prot, flags, fd, (off_t)offset);
}

int
sweet_os_munmap(void *address, size_t size)
{
	uint32_t type = 0;
	int found = 0;

	pthread_mutex_lock(&map_lock);
	for (int i = 0; i < MAP_TABLE; i++) {
		if (map_table[i].address == (uint64_t)(uintptr_t)address) {
			type = map_table[i].type;
			map_table[i].address = 0;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
	if (!found) {
		return munmap(address, size);
	}
	kern_return_t kr = IOConnectUnmapMemory64(connection, type, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
	return kr == 0 ? 0 : fail(EINVAL);
}
