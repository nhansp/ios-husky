/*
 * husky_panthor_uapi.h - what husky_panthor_drm.h (Linux include/uapi/drm/panthor_drm.h, MIT) needs from
 * drm.h, plus the DRM core GEM and syncobj argument structs. DRM_IO* give the bare ioctl number: the kernel
 * dispatches on it, userland builds full requests with its own drm.h.
 */
#ifndef HUSKY_PANTHOR_UAPI_H
#define HUSKY_PANTHOR_UAPI_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
typedef int64_t  __s64;

#define DRM_COMMAND_BASE        0x40
#define DRM_IO(nr)              (nr)
#define DRM_IOR(nr, type)       (nr)
#define DRM_IOW(nr, type)       (nr)
#define DRM_IOWR(nr, type)      (nr)

#define DRM_IOCTL_GEM_CLOSE                     0x09
#define DRM_IOCTL_SYNCOBJ_CREATE                0xBF
#define DRM_IOCTL_SYNCOBJ_DESTROY               0xC0
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD          0xC1
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE          0xC2
#define DRM_IOCTL_SYNCOBJ_WAIT                  0xC3
#define DRM_IOCTL_SYNCOBJ_RESET                 0xC4
#define DRM_IOCTL_SYNCOBJ_SIGNAL                0xC5
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT         0xCA
#define DRM_IOCTL_SYNCOBJ_QUERY                 0xCB
#define DRM_IOCTL_SYNCOBJ_TRANSFER              0xCC
#define DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL       0xCD

struct drm_gem_close {
	__u32 handle;
	__u32 pad;
};

#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
struct drm_syncobj_create {
	__u32 handle;
	__u32 flags;
};

struct drm_syncobj_destroy {
	__u32 handle;
	__u32 pad;
};

struct drm_syncobj_transfer {
	__u32 src_handle;
	__u32 dst_handle;
	__u64 src_point;
	__u64 dst_point;
	__u32 flags;
	__u32 pad;
};

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL         (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT  (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE   (1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE    (1 << 3)
/* timeout_nsec is relative here (the userland transport converts Linux's absolute CLOCK_MONOTONIC value) */
struct drm_syncobj_wait {
	__u64 handles;
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
	__u64 deadline_nsec;
};

struct drm_syncobj_timeline_wait {
	__u64 handles;
	__u64 points;
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
	__u64 deadline_nsec;
};

struct drm_syncobj_array {
	__u64 handles;
	__u32 count_handles;
	__u32 pad;
};

#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0)
struct drm_syncobj_timeline_array {
	__u64 handles;
	__u64 points;
	__u32 count_handles;
	__u32 flags;
};

/*
 * Husky's own ioctl: an IOSurface as a BO (in place of dma-buf import). The BO covers the surface's memory from its
 * base; offset returns the plane's offset in it. Number 0x60: past the panthor range (0x40 + id).
 */
#define DRM_IOCTL_HUSKY_BO_IMPORT_SURFACE       0x60
struct drm_husky_bo_import_surface {
	__u32 surface_id;
	__u32 plane;
	__u32 handle;           /* out */
	__u32 pad;
	__u64 size;             /* out */
	__u64 offset;           /* out */
};

/*
 * IOKit transport, connect type HUSKY_PNT_CONNECT_TYPE on the accelerator:
 *   selector 0 (ioctl):  scalar in {request}, struct in/out = the argument struct, scalar out {errno}.
 *                        The kernel dispatches on request & 0xff (the DRM ioctl number).
 *   selector 1 (mmap):   scalar in {bo handle}, scalar out {address, size}.
 *   selector 2 (munmap): scalar in {address}.
 */
#define HUSKY_PNT_CONNECT_TYPE  0x504eu
#define HUSKY_PNT_SEL_IOCTL     0u
#define HUSKY_PNT_SEL_MMAP      1u
#define HUSKY_PNT_SEL_MUNMAP    2u

#endif /* HUSKY_PANTHOR_UAPI_H */
