/*
 * libdrm for PanVK on iOS (see xf86drm.h).
 *
 * GPU requests: Linux's panfrost requests (0x40-0x4f) and GEM_CLOSE are calls of SweetMali's IOKit user client,
 * the request's number being the selector, its structure going in and coming back. Mesa knows the device by a
 * descriptor of /dev/null; there is one GPU and one connection to it in a process.
 *
 * Synchronisation objects live here, not in the kernel. That is exact for us: SweetMali runs every job chain to
 * its end before SUBMIT returns. So a syncobj is either signalled or has no fence at all, a SUBMIT signals its
 * out_sync when it returns, and an unsignalled one can only become signalled by another thread of this process
 * (a SUBMIT or a CPU signal), which a waiter with WAIT_FOR_SUBMIT waits for. Sync files (PanVK moves fences
 * between syncobjs through them) are descriptors of /dev/null with the state they carried.
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

#include "xf86drm.h"

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

/* ---- synchronisation objects ---- */

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
	trace("sweetdrm: out of syncobjs");
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
		trace("sweetdrm: out of sync files");
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
				/* one import per export (PanVK closes the descriptor right after) */
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
	/* The job chain has run to its end: its out_sync is signalled (struct drm_panfrost_submit: u32 at 20). */
	if (number == PANFROST_SUBMIT_NR) {
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
	(void)fd; (void)prime_fd; (void)handle;
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
	kr = IOConnectMapMemory64(connection, type, mach_task_self(), &address, &mapped, SWEET_ANYWHERE);
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
