/*
 * What Mesa's panfrost takes from libdrm's xf86drm.h, for a system without Linux's DRM. The
 * declarations are here, the functions in sweet.drm.c, which carries the requests to the kernel's
 * GPU service (SweetMali, iokit/Kernel/arm/SweetMali.cpp in the kernel) as calls of its user client.
 * Based on the shim of the HakiOS projects (Apple Public Source License 2.0, as the kernel side is).
 * The request numbers and their structures are Linux's (Mesa's include/drm-uapi).
 */
#ifndef SWEET_XF86DRM_H
#define SWEET_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* Buffers and fences as file descriptors: there are none here. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/*
 * Synchronisation objects, kept in this process: the kernel runs every job chain to its end before
 * the call that submitted it returns, so whatever a program waits for is already signalled.
 */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/*
 * Not libdrm's: a buffer's memory in this process, in place of mmap() on the device (pan_kmod.h is
 * patched to call it). `offset` is what the MMAP_BO request answered: the buffer's handle << 12.
 */
void *sweet_drm_mmap(size_t size, uint64_t offset);
int sweet_drm_munmap(void *address, size_t size);

#ifdef __cplusplus
}
#endif

#endif
