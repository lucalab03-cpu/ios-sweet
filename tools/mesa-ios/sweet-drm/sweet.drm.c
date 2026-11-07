/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU service as calls of
 * its IOKit user client. A request's number (the low byte of the ioctl code) is the call's selector,
 * its structure goes in and comes back as it is; a buffer's memory is the client memory of the type
 * that is the buffer's handle.
 *
 * Mesa knows its device by a file descriptor. It gets one that means nothing (a dup of /dev/null):
 * there is one GPU and one connection to it in a process.
 *
 * The IOKit calls are declared here, not included: the iOS SDK has no IOKitLib.h.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/ioccom.h>

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

#define SWEET_SERVICE           "SweetMali"
#define SWEET_ANYWHERE          1u
#define SWEET_RETURN_NO_MEMORY  0xe00002bdu
#define SWEET_RETURN_BAD_ARG    0xe00002c2u
#define SWEET_RETURN_TIMEOUT    0xe00002d6u
#define SWEET_RETURN_BUSY       0xe00002d5u
#define SWEET_RETURN_NOT_FOUND  0xe00002f0u
#define SWEET_RETURN_UNSUPPORTED 0xe00002c7u

static io_connect_t connection;
static int connection_fd = -1;
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;

static int
open_connection(void)
{
	io_service_t service;
	int fd = -1;

	pthread_mutex_lock(&open_lock);
	if (connection_fd < 0) {
		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(SWEET_SERVICE));
		if (service != 0) {
			if (IOServiceOpen(service, mach_task_self(), 0, &connection) == 0) {
				connection_fd = open("/dev/null", O_RDWR);
			}
			IOObjectRelease(service);
		}
	}
	if (connection_fd >= 0) {
		fd = dup(connection_fd);
	}
	pthread_mutex_unlock(&open_lock);
	return fd;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, "panfrost") != 0) {
		return -1;
	}
	return open_connection();
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (connection == 0) {
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

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
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

/* The service is what Linux's panfrost is called; the version is one that has everything Mesa asks of it. */
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
	(void)capability;
	*value = 0;
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close_request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_request);
}

void *
sweet_drm_mmap(size_t size, uint64_t offset)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)size;
	if (connection == 0 ||
	    IOConnectMapMemory64(connection, (uint32_t)(offset >> 12), mach_task_self(), &address, &mapped,
	    SWEET_ANYWHERE) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

/* Synchronisation objects: a handle is a number; everything is always signalled. */
#define SYNCOBJ_MAX 4096
static atomic_uchar syncobj_used[SYNCOBJ_MAX];

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	for (uint32_t i = 0; i < SYNCOBJ_MAX; i++) {
		unsigned char expected = 0;

		if (atomic_compare_exchange_strong(&syncobj_used[i], &expected, 1)) {
			*handle = i + 1;
			return 0;
		}
	}
	errno = ENOMEM;
	return -1;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	if (handle != 0 && handle <= SYNCOBJ_MAX) {
		atomic_store(&syncobj_used[handle - 1], 0);
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)count;
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)count;
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)num_handles;
	(void)timeout_nsec;
	(void)flags;
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)points;
	return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	(void)fd;
	(void)dst_handle;
	(void)dst_point;
	(void)src_handle;
	(void)src_point;
	(void)flags;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs: none of that here. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	return none();
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd; (void)handle;
	return none();
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd; (void)obj_fd; (void)handle;
	return none();
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd; (void)handle; (void)obj_fd;
	return none();
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd; (void)handle; (void)sync_file_fd;
	return none();
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd; (void)handle; (void)sync_file_fd;
	return none();
}
