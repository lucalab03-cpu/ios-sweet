/*
 * libdrm for Open Mapple's Metal driver on SweetMali (tools/mapple/README.md).
 *
 * Open Mapple's bundle (mapple_gpu_mali: Mesa's Gallium Panfrost + their Metal front end) talks to its kernel through
 * libdrm-xnu + vgpu (selectors of their MappleGPU service). This file takes the place of both (the workflow copies it
 * over libdrm-xnu/src/xf86drm_xnu.c and empties the vgpu sources), with the same entry points, on our kernel's SweetMali:
 *
 *   - Panfrost's requests (0x40-0x4f) and GEM_CLOSE are SweetMali's selectors, as in tools/panvk-ios/sweet-drm.
 *   - SweetMali runs every job chain to its end before SUBMIT returns, so syncobjs live here: a SUBMIT signals its
 *     out_sync when it returns (as in panvk-ios/sweet-drm). The XNU extras (snapshot, copy, release) are copies of state.
 *   - drmMap/drmUnmap (Mapple's Mesa patch 0003): MMAP_BO answers offset = handle << 12 and the buffer is mapped as the
 *     service's client memory of type `handle`, write-combined (patch 0127; boot-arg sweetmaliwc=0: cached). Mappings
 *     stay until the buffer is closed, as libdrm-xnu does: Panfrost unmaps every buffer it puts back in its cache.
 *   - IOSurfaces (vmg_drm_surface_fd) and user memory (drmUserMemoryFDXNU) become buffers through IMPORT_HOST (0x4e,
 *     patch 0128): the pages as mapped in this process, wired by the kernel and cleaned around each job (patch 0131).
 *     IMPORT_HOST needs whole pages: a plane that does not start on a page is refused, and the driver copies instead.
 *   - Not here: surface promises (Mapple's kernel minor 2) answer ENOTSUP, which the driver takes as "wait instead".
 *
 * Interface after Open Mapple's libdrm-xnu (GPL-3.0-or-later, viva-gpu by Nicola Leone Ciardi); the code is ours.
 * Log: /var/tmp/sweetmdrm.log (first 800 lines of the process).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/ioccom.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <IOSurface/IOSurfaceRef.h>
#include "xf86drm.h"
#include "libdrm/drm.h"
#include "libdrm/panfrost_drm.h"
#include "../../vgpu/vgpu.h"
#include "../../vgpu/iokit_compat.h"

extern int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

#define SWEET_SERVICE           "SweetMali"
#define SWEET_IMPORT_HOST_NR    0x4e
#define SWEET_WRITE_COMBINE     0x400u  /* kIOMapWriteCombineCache */
#define NR_GEM_CLOSE            0x09
#define NR_SUBMIT               0x40
#define NR_CREATE_BO            0x42
#define NR_MMAP_BO              0x43
#define NR_MADVISE              0x48
#define NR_SET_LABEL_BO         0x49

/* ---- log ---- */

static atomic_int log_lines;

static void
trace(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	if (atomic_fetch_add(&log_lines, 1) >= 800) {
		return;
	}
	n = snprintf(line, sizeof(line), "[%d %s] ", getpid(), getprogname());
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	int f = open("/var/tmp/sweetmdrm.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (f >= 0) {
		fchmod(f, 0666);
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
static atomic_ullong calls_made, jobs_made;

static int
connect_service(void)
{
	pthread_mutex_lock(&open_lock);
	if (connection == 0) {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SWEET_SERVICE));
		if (service != 0) {
			kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
			if (kr != 0) {
				connection = 0;
			}
			trace("open %s -> %#x", SWEET_SERVICE, kr);
			IOObjectRelease(service);
		} else {
			trace("no %s service", SWEET_SERVICE);
		}
	}
	pthread_mutex_unlock(&open_lock);
	return connection != 0 ? 0 : -1;
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (connection == 0 && connect_service() != 0) {
		return ENODEV;
	}
	atomic_fetch_add_explicit(&calls_made, 1, memory_order_relaxed);
	result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoResources:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

/* ---- vgpu: what the Metal driver asks of Mapple's client directly ---- */

int
vgpu_open(void)
{
	if (connect_service() != 0) {
		return -ENODEV;
	}
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	return fd >= 0 ? fd : -errno;
}

int
vgpu_close(int fd)
{
	return close(fd) == 0 ? 0 : -errno;
}

bool
vgpu_is_fd(int fd)
{
	return fd >= 0 && connection != 0;
}

int
vgpu_ioctl(int fd, enum vgpu_selector sel, void *arg)
{
	(void)fd; (void)sel; (void)arg;
	return -ENOSYS;
}

void
vgpu_counters(uint64_t calls[VGPU_SEL_COUNT], uint64_t *jobs)
{
	if (calls != NULL) {
		memset(calls, 0, sizeof(uint64_t) * VGPU_SEL_COUNT);
		calls[0] = atomic_load_explicit(&calls_made, memory_order_relaxed);
	}
	if (jobs != NULL) {
		*jobs = atomic_load_explicit(&jobs_made, memory_order_relaxed);
	}
}

void *
vgpu_mmap(int fd, uint64_t token, size_t size)
{
	(void)fd; (void)token; (void)size;
	errno = ENOSYS;
	return NULL;
}

int
vgpu_munmap(int fd, void *addr, size_t size)
{
	(void)fd; (void)addr; (void)size;
	return -ENOSYS;
}

const char *
vgpu_backend_name(void)
{
	return "sweetmali";
}

void
vgpu_mock_set_submit_hook(vgpu_mock_submit_hook hook, void *ctx)
{
	(void)hook; (void)ctx;
}

size_t
vgpu_mock_mappings(void)
{
	return 0;
}

/* ---- synchronisation objects (in this process; see the top) ---- */

#define SYNCOBJ_MAX 4096
static unsigned char sync_used[SYNCOBJ_MAX + 1], sync_signalled[SYNCOBJ_MAX + 1];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_changed = PTHREAD_COND_INITIALIZER;

#define SYNC_FILES 256
static struct { int fd; unsigned char signalled; } sync_files[SYNC_FILES];

static int
valid(uint32_t h)
{
	return h != 0 && h <= SYNCOBJ_MAX && sync_used[h];
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
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
	trace("out of syncobjs");
	return fail(ENOMEM);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	int ok;

	(void)fd;
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

static uint64_t
now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

/* 0, or -errno (ETIME when the time ran out), as libdrm. */
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
		/* No fence: Linux says EINVAL unless WAIT_FOR_SUBMIT, then only another thread here can signal it. */
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
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (points != NULL && points[i] != 0) {
			return fail(EINVAL);
		}
	}
	return drmSyncobjSignal(fd, handles, count);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	(void)fd;
	(void)flags;
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

/* dst takes src's state */
static int
copy_state(uint32_t dst, uint32_t src)
{
	pthread_mutex_lock(&sync_lock);
	if (!valid(dst) || !valid(src)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	sync_signalled[dst] = sync_signalled[src];
	pthread_cond_broadcast(&sync_changed);
	pthread_mutex_unlock(&sync_lock);
	return 0;
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
	return copy_state(dst_handle, src_handle);
}

int
drmSyncobjSnapshotXNU(int fd, uint32_t src, uint32_t *handle)
{
	uint32_t h;

	if (drmSyncobjCreate(fd, 0, &h) != 0) {
		return -errno;
	}
	if (copy_state(h, src) != 0) {
		int e = errno;
		drmSyncobjDestroy(fd, h);
		errno = e;
		return -e;
	}
	*handle = h;
	return 0;
}

int
drmSyncobjReleaseXNU(int fd, uint32_t handle)
{
	return drmSyncobjDestroy(fd, handle) == 0 ? 0 : -errno;
}

int
drmSyncobjCopyXNU(int fd, uint32_t dst, uint32_t src)
{
	(void)fd;
	return copy_state(dst, src) == 0 ? 0 : -errno;
}

int
drmSurfacePromiseXNU(int fd, const uint32_t *surface_ids, uint32_t count, uint32_t timeout_ms, uint32_t *handle)
{
	(void)fd; (void)surface_ids; (void)count; (void)timeout_ms; (void)handle;
	errno = ENOTSUP;
	return -ENOTSUP;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	pthread_mutex_lock(&sync_lock);
	if (!valid(handle)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	int f = open("/dev/null", O_RDONLY | O_CLOEXEC), slot = -1;
	if (f < 0) {
		pthread_mutex_unlock(&sync_lock);
		return -1;
	}
	/* an entry with this number is stale (its descriptor was closed without an import): replace it */
	for (int i = 0; i < SYNC_FILES && slot < 0; i++) {
		if (sync_files[i].fd == f) {
			slot = i;
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
		trace("out of sync files");
		return fail(ENOMEM);
	}
	sync_files[slot].fd = f;
	sync_files[slot].signalled = sync_signalled[handle];
	pthread_mutex_unlock(&sync_lock);
	*sync_file_fd = f;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	unsigned char state = 1;        /* -1: "already signalled" */

	(void)fd;
	pthread_mutex_lock(&sync_lock);
	if (!valid(handle)) {
		pthread_mutex_unlock(&sync_lock);
		return fail(EINVAL);
	}
	if (sync_file_fd >= 0) {
		int found = 0;
		for (int i = 0; i < SYNC_FILES; i++) {
			if (sync_files[i].fd == sync_file_fd) {
				state = sync_files[i].signalled;
				sync_files[i].fd = 0;
				found = 1;
				break;
			}
		}
		if (!found) {
			pthread_mutex_unlock(&sync_lock);
			return fail(EINVAL);
		}
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

/* ---- buffers: imported ones and CPU mappings ---- */

/* Buffers made by IMPORT_HOST: their CPU address is the caller's own. surface: the IOSurface kept alive meanwhile. */
#define BO_TABLE 8192
static struct {
	uint8_t imported;
	void *cpu;
	uint64_t size, gpu;             /* gpu, size: every buffer, for decoding a failed job chain */
	IOSurfaceRef surface;
	uint32_t surface_id, plane;
} bos[BO_TABLE];
static pthread_mutex_t bo_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
imported(uint32_t handle)
{
	bool yes;

	pthread_mutex_lock(&bo_lock);
	yes = handle < BO_TABLE && bos[handle].imported;
	pthread_mutex_unlock(&bo_lock);
	return yes;
}

/* Mappings: kept after drmUnmap (idle) until the buffer is closed, then unmapped for real. */
typedef struct {
	void *addr;
	size_t size;
	uint32_t handle;
	bool idle, kernel;
} Mapping;
static Mapping *maps;
static size_t map_count, map_cap;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t
map_options(void)
{
	static int wc = -1;

	if (wc < 0) {
		char args[1024] = "";
		size_t len = sizeof(args) - 1;
		wc = !(sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0 && strstr(args, "sweetmaliwc=0") != NULL);
		trace("buffers are mapped %s", wc ? "write-combined" : "cached (sweetmaliwc=0)");
	}
	return kIOMapAnywhere | (wc ? SWEET_WRITE_COMBINE : 0);
}

static void
remember_mapping(void *addr, size_t size, uint32_t handle, bool kernel)
{
	pthread_mutex_lock(&map_lock);
	if (map_count == map_cap) {
		size_t cap = map_cap ? map_cap * 2 : 256;
		Mapping *m = realloc(maps, cap * sizeof(*m));
		if (m == NULL) {
			pthread_mutex_unlock(&map_lock);
			return;
		}
		maps = m;
		map_cap = cap;
	}
	maps[map_count++] = (Mapping){ addr, size, handle, false, kernel };
	pthread_mutex_unlock(&map_lock);
}

int
drmMap(int fd, drm_handle_t token, drmSize size, drmAddressPtr address)
{
	uint32_t handle = (uint32_t)((uint64_t)token >> 12);
	mach_vm_address_t at = 0;
	mach_vm_size_t mapped = 0;
	kern_return_t kr;

	(void)fd;
	pthread_mutex_lock(&map_lock);
	for (size_t i = 0; i < map_count; i++) {
		if (maps[i].idle && maps[i].handle == handle && maps[i].size == size) {
			maps[i].idle = false;
			*address = maps[i].addr;
			pthread_mutex_unlock(&map_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&map_lock);

	pthread_mutex_lock(&bo_lock);
	void *cpu = handle < BO_TABLE && bos[handle].imported ? bos[handle].cpu : NULL;
	pthread_mutex_unlock(&bo_lock);
	if (cpu != NULL) {
		remember_mapping(cpu, size, handle, false);
		*address = cpu;
		return 0;
	}
	if (connection == 0) {
		return -ENODEV;
	}
	kr = IOConnectMapMemory64(connection, handle, mach_task_self(), &at, &mapped, map_options());
	if (kr != 0 || mapped < size) {
		trace("map buffer %u size %#llx -> %#x (mapped %#llx)", handle, (unsigned long long)size, kr,
		    (unsigned long long)mapped);
		if (kr == 0) {
			IOConnectUnmapMemory64(connection, handle, mach_task_self(), at);
		}
		errno = ENOMEM;
		return -ENOMEM;
	}
	remember_mapping((void *)(uintptr_t)at, size, handle, true);
	*address = (void *)(uintptr_t)at;
	return 0;
}

int
drmUnmap(drmAddress address, drmSize size)
{
	(void)size;
	pthread_mutex_lock(&map_lock);
	for (size_t i = 0; i < map_count; i++) {
		if (maps[i].addr == address && !maps[i].idle) {
			maps[i].idle = true;
			pthread_mutex_unlock(&map_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&map_lock);
	/* Panfrost aborts on a failed unmap: an address we never gave out is logged and let go */
	trace("unmap of unknown address %p", address);
	return 0;
}

/* Before the buffer goes: IOConnectUnmapMemory64 needs the handle alive. */
static void
drop_mappings(uint32_t handle)
{
	pthread_mutex_lock(&map_lock);
	for (size_t i = 0; i < map_count;) {
		if (maps[i].handle != handle) {
			i++;
			continue;
		}
		Mapping m = maps[i];
		maps[i] = maps[--map_count];
		if (m.kernel && connection != 0) {
			IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)m.addr);
		}
	}
	pthread_mutex_unlock(&map_lock);
}

static int
close_buffer(uint32_t handle)
{
	struct drm_gem_close c = { .handle = handle };
	IOSurfaceRef surface = NULL;

	drop_mappings(handle);
	int error = call(NR_GEM_CLOSE, &c, sizeof(c));
	pthread_mutex_lock(&bo_lock);
	if (handle < BO_TABLE) {
		surface = bos[handle].imported ? bos[handle].surface : NULL;
		memset(&bos[handle], 0, sizeof(bos[handle]));
	}
	pthread_mutex_unlock(&bo_lock);
	if (surface != NULL) {
		CFRelease(surface);
	}
	return error;
}

/* IMPORT_HOST: whole pages of this process become a buffer. 0 or errno. */
static int
import_host(void *address, uint64_t size, uint32_t *handle, uint64_t *gpu)
{
	struct { uint64_t address, size; uint32_t handle, pad; uint64_t offset; } r = { (uint64_t)(uintptr_t)address, size, 0, 0, 0 };
	int error = call(SWEET_IMPORT_HOST_NR, &r, sizeof(r));
	trace("IMPORT_HOST %p size %#llx -> err %d, handle %u gpu %#llx", address, (unsigned long long)size, error, r.handle,
	    (unsigned long long)r.offset);
	if (error == 0) {
		*handle = r.handle;
		*gpu = r.offset;
	}
	return error;
}

/*
 * A job chain the GPU did not finish (SweetMali answers SUBMIT with kIOReturnIOError; the kernel log has the
 * exception code): Mesa's own decoder prints the chain and its descriptors, with the faulting job's status, into
 * /var/tmp/sweetpandecode.<process>... (the outbox takes /var/tmp/sweet*). Every buffer of this process is shown to
 * it at its GPU address; buffers not mapped yet are mapped for it (and kept, as drmMap keeps them). First 6 per process.
 * Boot 2026-11-23: backboardd's fragment jobs ended with 0x5a (ADDR_RANGE_FAULT) and 0x5b (IMPRECISE_FAULT), MMU
 * fault 0.
 */
struct pandecode_context;
struct pandecode_context *pandecode_create_context(bool to_stderr);
void pandecode_inject_mmap(struct pandecode_context *ctx, uint64_t gpu_va, void *cpu, unsigned sz, const char *name);
void pandecode_jc(struct pandecode_context *ctx, uint64_t jc_gpu_va, unsigned gpu_id);
void pandecode_destroy_context(struct pandecode_context *ctx);

static void *
cpu_of(uint32_t handle, uint64_t size)
{
	void *cpu = NULL;

	pthread_mutex_lock(&bo_lock);
	if (bos[handle].imported) {
		cpu = bos[handle].cpu;
	}
	pthread_mutex_unlock(&bo_lock);
	if (cpu != NULL) {
		return cpu;
	}
	pthread_mutex_lock(&map_lock);
	for (size_t i = 0; i < map_count && cpu == NULL; i++) {
		if (maps[i].handle == handle && maps[i].size >= size) {
			cpu = maps[i].addr;
		}
	}
	pthread_mutex_unlock(&map_lock);
	if (cpu == NULL) {
		void *at = NULL;
		if (drmMap(-1, (drm_handle_t)((uint64_t)handle << 12), size, &at) == 0) {
			drmUnmap(at, size);     /* kept, idle, until the buffer is closed */
			cpu = at;
		}
	}
	return cpu;
}

static void
decode_failed_chain(uint64_t jc, uint32_t requirements)
{
	static atomic_int decoded;
	static unsigned gpu_id;

	if (atomic_fetch_add(&decoded, 1) >= 6 || getenv("SWEET_NO_DECODE") != NULL) {
		return;
	}
	if (gpu_id == 0) {
		struct { uint32_t param, pad; uint64_t value; } p = { 0, 0, 0 };    /* PANFROST_PARAM_GPU_PROD_ID */
		gpu_id = call(0x44, &p, sizeof(p)) == 0 && p.value ? (unsigned)(p.value << 16) : 0x74020000u;
	}
	char name[200];
	snprintf(name, sizeof(name), "/var/tmp/sweetpandecode.%s", getprogname());
	setenv("PANDECODE_DUMP_FILE", name, 1);
	struct pandecode_context *ctx = pandecode_create_context(false);
	if (ctx == NULL) {
		return;
	}
	unsigned shown = 0;
	for (uint32_t h = 1; h < BO_TABLE; h++) {
		pthread_mutex_lock(&bo_lock);
		uint64_t gpu = bos[h].gpu, size = bos[h].size;
		pthread_mutex_unlock(&bo_lock);
		if (gpu == 0 || size == 0) {
			continue;
		}
		void *cpu = cpu_of(h, size);
		if (cpu != NULL) {
			pandecode_inject_mmap(ctx, gpu, cpu, (unsigned)size, NULL);
			shown++;
		}
	}
	trace("decoding failed chain %#llx (requirements %#x, gpu id %#x, %u buffers) into %s.*", (unsigned long long)jc,
	    requirements, gpu_id, shown, name);
	pandecode_jc(ctx, jc, gpu_id);
	pandecode_destroy_context(ctx);
}

/* ---- requests ---- */

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	*value = capability == DRM_CAP_SYNCOBJ ? 1 : 0;
	return 0;
}

static int
local_ioctl(unsigned long request, void *arg)
{
	switch (request) {
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap *c = arg;
		uint64_t value = 0;
		(void)drmGetCap(-1, c->capability, &value);
		c->value = value;
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_CREATE: {
		struct drm_syncobj_create *c = arg;
		return drmSyncobjCreate(-1, c->flags, &c->handle);
	}
	case DRM_IOCTL_SYNCOBJ_DESTROY: {
		struct drm_syncobj_destroy *d = arg;
		return drmSyncobjDestroy(-1, d->handle);
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait *w = arg;
		int r = wait_syncobjs((const uint32_t *)(uintptr_t)w->handles, w->count_handles, w->timeout_nsec, w->flags,
		    &w->first_signaled);
		return r == 0 ? 0 : fail(-r);
	}
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array *a = arg;
		return set_state((const uint32_t *)(uintptr_t)a->handles, a->count_handles, request == DRM_IOCTL_SYNCOBJ_SIGNAL);
	}
	case DRM_IOCTL_SYNCOBJ_TRANSFER: {
		struct drm_syncobj_transfer *t = arg;
		return drmSyncobjTransfer(-1, t->dst_handle, t->dst_point, t->src_handle, t->src_point, t->flags);
	}
	default:
		trace("unsupported request %#lx", request);
		return fail(ENOSYS);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	int error;

	(void)fd;
	if (number == NR_GEM_CLOSE) {
		error = close_buffer(((struct drm_gem_close *)arg)->handle);
		return error == 0 ? 0 : fail(error);
	}
	if (number < 0x40 || number > 0x4f) {
		return local_ioctl(request, arg);
	}
	switch (number) {
	case NR_SET_LABEL_BO:           /* labels only name buffers in debug output */
		return 0;
	case NR_MADVISE:                /* SweetMali never purges a buffer */
		((struct drm_panfrost_madvise *)arg)->retained = 1;
		return 0;
	case NR_MMAP_BO: {              /* imported buffers are mapped already: the offset still names the handle */
		struct drm_panfrost_mmap_bo *m = arg;
		if (imported(m->handle)) {
			m->offset = (uint64_t)m->handle << 12;
			return 0;
		}
		break;
	}
	case SWEET_IMPORT_HOST_NR:      /* ours, never Mesa's */
		return fail(ENOSYS);
	default:
		break;
	}
	error = call(number, arg, IOCPARM_LEN(request));
	if (number == NR_CREATE_BO && error == 0) {
		const struct drm_panfrost_create_bo *c = arg;
		if (c->handle < BO_TABLE) {
			pthread_mutex_lock(&bo_lock);
			bos[c->handle].gpu = c->offset;
			bos[c->handle].size = c->size;
			pthread_mutex_unlock(&bo_lock);
		}
	}
	if (number == NR_SUBMIT && error != 0) {
		const struct drm_panfrost_submit *sub = arg;
		decode_failed_chain(sub->jc, sub->requirements);
	}
	if (number == NR_CREATE_BO) {
		const struct drm_panfrost_create_bo *c = arg;
		if (error != 0 || atomic_load(&log_lines) < 200) {
			trace("CREATE_BO size %#x flags %#x -> err %d, handle %u gpu %#llx", c->size, c->flags, error, c->handle,
			    (unsigned long long)c->offset);
		}
	} else if (error != 0) {
		trace("request %#x (%lu bytes) -> err %d", number, (unsigned long)IOCPARM_LEN(request), error);
	}
	if (error != 0) {
		return fail(error);
	}
	/* The job chain has run to its end: its out_sync is signalled. */
	if (number == NR_SUBMIT) {
		uint32_t out = ((const struct drm_panfrost_submit *)arg)->out_sync;
		atomic_fetch_add_explicit(&jobs_made, 1, memory_order_relaxed);
		if (out != 0) {
			(void)set_state(&out, 1, 1);
		}
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	(void)fd;
	int error = close_buffer(handle);
	return error == 0 ? 0 : fail(error);
}

/* panfrost 1.3: no JM contexts, SYNC_BO or WB mappings (1.5/1.6), which SweetMali has not got. */
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
		version->date = strdup("20261123");
		version->date_len = 8;
		version->desc = strdup("Mali-G52 on SweetMali");
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

/* ---- "dma-buf" descriptors: IOSurface planes, user memory, buffers of this process ---- */

enum { FD_SURFACE = 1, FD_USER, FD_BO };
#define SPECIAL_FDS 256
static struct {
	int fd, kind;
	uint32_t surface_id, plane, handle;
	uint64_t addr, size;
} special[SPECIAL_FDS];
static pthread_mutex_t special_lock = PTHREAD_MUTEX_INITIALIZER;

/* Mesa asks lseek(fd, 0, SEEK_END) for an imported buffer's size: a sparse unlinked file of that size. */
static int
size_fd(uint64_t size)
{
	char path[1024];
	const char *tmp = getenv("TMPDIR");

	snprintf(path, sizeof(path), "%s/sweet-mdrm-XXXXXX", tmp != NULL && *tmp ? tmp : "/tmp");
	int fd = mkstemp(path);
	if (fd < 0) {
		return -1;
	}
	unlink(path);
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
register_fd(int fd, int kind, uint32_t surface_id, uint32_t plane, uint32_t handle, uint64_t addr, uint64_t size)
{
	pthread_mutex_lock(&special_lock);
	for (int i = 0; i < SPECIAL_FDS; i++) {
		if (special[i].fd <= 0 || special[i].fd == fd) {
			special[i].fd = fd;
			special[i].kind = kind;
			special[i].surface_id = surface_id;
			special[i].plane = plane;
			special[i].handle = handle;
			special[i].addr = addr;
			special[i].size = size;
			pthread_mutex_unlock(&special_lock);
			return fd;
		}
	}
	pthread_mutex_unlock(&special_lock);
	close(fd);
	trace("out of descriptor slots");
	errno = EMFILE;
	return -1;
}

/* The plane's first byte and the bytes from there to the surface's end. */
static uint8_t *
plane_start(IOSurfaceRef s, unsigned plane, uint64_t *bytes)
{
	uint8_t *base = IOSurfaceGetBaseAddress(s);
	uint8_t *start = IOSurfaceGetPlaneCount(s) ? IOSurfaceGetBaseAddressOfPlane(s, plane) : base;
	size_t total = IOSurfaceGetAllocSize(s);

	*bytes = base != NULL && start != NULL ? total - (uint64_t)(start - base) : 0;
	return start;
}

int
vmg_drm_surface_fd(int device_fd, uint32_t surface_id, unsigned plane)
{
	(void)device_fd;
	IOSurfaceRef s = IOSurfaceLookup(surface_id);
	uint64_t bytes = 0;

	if (s == NULL) {
		return -1;
	}
	(void)plane_start(s, plane, &bytes);
	CFRelease(s);
	int fd = size_fd(bytes);
	return fd < 0 ? -1 : register_fd(fd, FD_SURFACE, surface_id, plane, 0, 0, bytes);
}

int
drmUserMemoryFDXNU(int device_fd, uint64_t addr, uint64_t size)
{
	(void)device_fd;
	int fd = size_fd(size);
	return fd < 0 ? -1 : register_fd(fd, FD_USER, 0, 0, 0, addr, size);
}

int
vmg_drm_bo_fd(int device_fd, uint32_t handle)
{
	(void)device_fd;
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return fd < 0 ? -1 : register_fd(fd, FD_BO, 0, 0, handle, 0, 0);
}

void
vmg_drm_surface_fd_close(int fd)
{
	pthread_mutex_lock(&special_lock);
	for (int i = 0; i < SPECIAL_FDS; i++) {
		if (special[i].fd == fd) {
			special[i].fd = 0;
		}
	}
	pthread_mutex_unlock(&special_lock);
	if (fd >= 0) {
		close(fd);
	}
}

static int
import_surface(uint32_t surface_id, uint32_t plane, uint32_t *handle)
{
	const uint64_t page = (uint64_t)getpagesize();
	uint64_t bytes = 0;

	/* the same plane again: the same buffer, as dma-buf imports are on Linux (Panfrost orders its jobs by buffer) */
	pthread_mutex_lock(&bo_lock);
	for (uint32_t h = 1; h < BO_TABLE; h++) {
		if (bos[h].imported && bos[h].surface != NULL && bos[h].surface_id == surface_id && bos[h].plane == plane) {
			*handle = h;
			pthread_mutex_unlock(&bo_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&bo_lock);

	IOSurfaceRef s = IOSurfaceLookup(surface_id);
	if (s == NULL) {
		return ENOENT;
	}
	uint8_t *start = plane_start(s, plane, &bytes);
	if (start == NULL || bytes == 0 || ((uintptr_t)start & (page - 1)) != 0) {
		trace("IOSurface %u plane %u at %p: not on a page, copied", surface_id, plane, start);
		CFRelease(s);
		return ENOTSUP;
	}
	uint64_t size = (bytes + page - 1) & ~(page - 1), gpu = 0;
	uint32_t h = 0;
	int error = import_host(start, size, &h, &gpu);
	if (error != 0 || h >= BO_TABLE) {
		if (error == 0) {
			struct drm_gem_close c = { .handle = h };
			(void)call(NR_GEM_CLOSE, &c, sizeof(c));
			error = ENOMEM;
		}
		CFRelease(s);
		return error;
	}
	pthread_mutex_lock(&bo_lock);
	bos[h].imported = 1;
	bos[h].cpu = start;
	bos[h].size = size;
	bos[h].gpu = gpu;
	bos[h].surface = s;             /* our reference, dropped when the buffer is closed */
	bos[h].surface_id = surface_id;
	bos[h].plane = plane;
	pthread_mutex_unlock(&bo_lock);
	*handle = h;
	return 0;
}

static int
import_user(uint64_t addr, uint64_t size, uint32_t *handle)
{
	uint32_t h = 0;
	uint64_t gpu = 0;
	int error = import_host((void *)(uintptr_t)addr, size, &h, &gpu);

	if (error != 0) {
		return error;
	}
	if (h >= BO_TABLE) {
		struct drm_gem_close c = { .handle = h };
		(void)call(NR_GEM_CLOSE, &c, sizeof(c));
		return ENOMEM;
	}
	pthread_mutex_lock(&bo_lock);
	bos[h].imported = 1;
	bos[h].cpu = (void *)(uintptr_t)addr;
	bos[h].size = size;
	bos[h].gpu = gpu;
	pthread_mutex_unlock(&bo_lock);
	*handle = h;
	return 0;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	int kind = 0;
	uint32_t surface_id = 0, plane = 0, bo = 0;
	uint64_t addr = 0, size = 0;

	(void)fd;
	pthread_mutex_lock(&special_lock);
	for (int i = 0; i < SPECIAL_FDS; i++) {
		if (special[i].fd == prime_fd && prime_fd > 0) {
			kind = special[i].kind;
			surface_id = special[i].surface_id;
			plane = special[i].plane;
			bo = special[i].handle;
			addr = special[i].addr;
			size = special[i].size;
			break;
		}
	}
	pthread_mutex_unlock(&special_lock);

	int error;
	switch (kind) {
	case FD_BO:
		*handle = bo;
		return 0;
	case FD_SURFACE:
		error = import_surface(surface_id, plane, handle);
		break;
	case FD_USER:
		error = import_user(addr, size, handle);
		break;
	default:
		error = EBADF;
		break;
	}
	if (error != 0) {
		errno = error;
		return -error;
	}
	return 0;
}

/* Buffers as descriptors for other programs: not here (sharing goes through IOSurfaces). */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;
	return -ENOSYS;
}

/* ---- the device, for code that enumerates (Gallium's Panfrost opens the fd it is given) ---- */

int
drmGetMagic(int fd, drm_magic_t *magic)
{
	(void)fd;
	(void)magic;
	return fail(EINVAL);
}

char *
drmGetDeviceNameFromFd2(int fd)
{
	(void)fd;
	return strdup("/dev/null");
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
	(void)fd;
	return strdup("/dev/null");
}

int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}
