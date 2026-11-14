/*
 * libdrm's xf86drm.h for PanVK on iOS (Mesa 26.2): what Mesa's Vulkan runtime, its WSI and PanVK take from
 * libdrm, on a system without Linux's DRM. The functions are in sweet.drm.c. Requests for the GPU go to the
 * kernel's SweetMali service; DRM's synchronisation objects live in this process (see sweet.drm.c).
 * A fork of tools/mesa-ios/sweet-drm (Gallium on Mesa 25.0.7), which stays as it is.
 * The request numbers and structures are Linux's (Mesa's include/drm-uapi).
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
#define DRM_NODE_CONTROL        1
#define DRM_NODE_RENDER         2
#define DRM_NODE_MAX            3

#define DRM_BUS_PCI             0
#define DRM_BUS_USB             1
#define DRM_BUS_PLATFORM        2
#define DRM_BUS_HOST1X          3

#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR
#define DRM_DEVICE_GET_PCI_REVISION (1 << 0)

typedef unsigned int drm_magic_t;

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPciBusInfo {
	uint16_t domain;
	uint8_t bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
	uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmUsbBusInfo {
	uint8_t bus, dev;
} drmUsbBusInfo, *drmUsbBusInfoPtr;

typedef struct _drmUsbDeviceInfo {
	uint16_t vendor, product;
} drmUsbDeviceInfo, *drmUsbDeviceInfoPtr;

#define DRM_PLATFORM_DEVICE_NAME_LEN 512
typedef struct _drmPlatformBusInfo {
	char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
	char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

#define DRM_HOST1X_DEVICE_NAME_LEN 512
typedef struct _drmHost1xBusInfo {
	char fullname[DRM_HOST1X_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmHost1xDeviceInfo {
	char **compatible;
} drmHost1xDeviceInfo, *drmHost1xDeviceInfoPtr;

typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union {
		drmPciBusInfoPtr pci;
		drmUsbBusInfoPtr usb;
		drmPlatformBusInfoPtr platform;
		drmHost1xBusInfoPtr host1x;
	} businfo;
	union {
		drmPciDeviceInfoPtr pci;
		drmUsbDeviceInfoPtr usb;
		drmPlatformDeviceInfoPtr platform;
		drmHost1xDeviceInfoPtr host1x;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmGetMagic(int fd, drm_magic_t *magic);
int drmCloseBufferHandle(int fd, uint32_t handle);

/*
 * The one device: the Mali through SweetMali. Its "render node" is /dev/null: PanVK opens that path and gets a
 * descriptor that means nothing; every request goes through the process's one IOKit connection, whatever the fd.
 */
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
char *drmGetDeviceNameFromFd2(int fd);
char *drmGetRenderDeviceNameFromFd(int fd);
int drmGetNodeTypeFromFd(int fd);

/* Buffers as file descriptors (dma-buf): none here. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* Synchronisation objects, binary, kept in this process (sweet.drm.c says why that is right). */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/*
 * Not libdrm's: util/os_mman.h is patched to call these (SWEET_IOS). A mapping of the device (a descriptor of
 * /dev/null, not MAP_ANON) becomes the service's client memory; everything else is plain mmap()/munmap().
 */
void *sweet_os_mmap(void *address, size_t size, int prot, int flags, int fd, long long offset);
int sweet_os_munmap(void *address, size_t size);

#ifdef __cplusplus
}
#endif

#endif
