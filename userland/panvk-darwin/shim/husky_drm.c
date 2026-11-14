/*
 * husky_drm.c - the libdrm entry points panvk uses, carried over the HuskyPanthorUserClient IOKit transport
 * (include/husky_panthor_uapi.h). The wire format is the Linux panthor/DRM ioctl structs; the syncobj wrappers
 * follow libdrm's xf86drm.c.
 */
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "husky_drm.h"
#include "xf86drm.h"

#define DRM_NR(req)             ((req) & 0xff)
/* Transport constants from include/husky_panthor_uapi.h, which cannot be included next to the real drm.h. */
#define HUSKY_PNT_CONNECT_TYPE  0x504eu
#define HUSKY_PNT_SEL_IOCTL     0u
#define HUSKY_PNT_SEL_MMAP      1u
#define HUSKY_PNT_SEL_MUNMAP    2u
/* The same three selectors tunnelled through IOGPUDeviceUserClient (connect type 1), which the 26.5 sandbox admits. */
#define HUSKY_TUNNEL_CONNECT_TYPE 1u
#define HUSKY_TUNNEL_SEL_BASE   56u
#define DRM_IOCTL_HUSKY_BO_IMPORT_SURFACE_NR 0x60
/* struct drm_husky_bo_import_surface from include/husky_panthor_uapi.h. */
struct drm_husky_import {
	uint32_t surface_id, plane, handle, pad;
	uint64_t size, offset;
};
#define HUSKY_IMPORT_REQUEST    (DRM_IOWR(DRM_IOCTL_HUSKY_BO_IMPORT_SURFACE_NR, struct drm_husky_import))
#define MAX_FDS                 4096
/* DRM_PANTHOR_USER_MMIO_OFFSET_64BIT: file offsets at or above this are the LATEST_FLUSH_ID page, not BOs. */
#define USER_MMIO_OFFSET        (1ull << 56)
#define memclear(s)             memset(&s, 0, sizeof(s))

static _Atomic io_connect_t conns[MAX_FDS];
static uint32_t sel_base[MAX_FDS];

static io_connect_t
conn_for_fd(int fd)
{
	if (fd < 0 || fd >= MAX_FDS) {
		return MACH_PORT_NULL;
	}
	return atomic_load_explicit(&conns[fd], memory_order_acquire);
}

int
husky_drm_open(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
	if (svc == IO_OBJECT_NULL) {
		errno = ENODEV;
		return -1;
	}
	io_connect_t conn = MACH_PORT_NULL;
	uint32_t base = HUSKY_TUNNEL_SEL_BASE;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), HUSKY_TUNNEL_CONNECT_TYPE, &conn);
	if (kr == KERN_SUCCESS) {
		/* An IOGPUDeviceUserClient that does not forward selector 56 rejects the call itself. */
		struct drm_gem_close probe = { 0, 0 };
		uint64_t in = DRM_IOCTL_GEM_CLOSE, err = 0;
		uint32_t nout = 1;
		size_t osz = sizeof(probe);
		if (IOConnectCallMethod(conn, base + HUSKY_PNT_SEL_IOCTL, &in, 1, &probe, sizeof(probe), &err, &nout, &probe,
		    &osz) != KERN_SUCCESS) {
			IOServiceClose(conn);
			kr = KERN_FAILURE;
		}
	}
	if (kr != KERN_SUCCESS) {
		base = 0;
		kr = IOServiceOpen(svc, mach_task_self(), HUSKY_PNT_CONNECT_TYPE, &conn);
	}
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		errno = kr == kIOReturnNotPermitted ? EPERM : ENODEV;
		return -1;
	}
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0 || fd >= MAX_FDS) {
		if (fd >= 0) {
			close(fd);
			errno = EMFILE;
		}
		IOServiceClose(conn);
		return -1;
	}
	sel_base[fd] = base;
	atomic_store_explicit(&conns[fd], conn, memory_order_release);
	return fd;
}

int
husky_drm_is_fd(int fd)
{
	return conn_for_fd(fd) != MACH_PORT_NULL;
}

struct bo_map {
	io_connect_t conn;
	uint32_t base;
	uint32_t handle;
	uint32_t refs;
	uint64_t addr;
	uint64_t size;
};

static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;
static struct bo_map *maps;
static size_t n_maps, cap_maps;

static void
drop_maps_for_conn(io_connect_t conn)
{
	pthread_mutex_lock(&maps_lock);
	for (size_t i = 0; i < n_maps;) {
		if (maps[i].conn == conn) {
			maps[i] = maps[--n_maps];
		} else {
			i++;
		}
	}
	pthread_mutex_unlock(&maps_lock);
}

void
husky_drm_close(int fd)
{
	io_connect_t conn = conn_for_fd(fd);
	if (conn != MACH_PORT_NULL) {
		atomic_store_explicit(&conns[fd], MACH_PORT_NULL, memory_order_release);
		drop_maps_for_conn(conn);
		IOServiceClose(conn);
	}
	close(fd);
}

static int
transport_ioctl(io_connect_t conn, uint32_t base, unsigned long request, void *arg, size_t size)
{
	uint64_t in = request, err = 0;
	uint32_t nout = 1;
	size_t osize = size;
	kern_return_t kr = IOConnectCallMethod(conn, base + HUSKY_PNT_SEL_IOCTL, &in, 1, arg, size, &err, &nout, arg, &osize);
	if (kr != KERN_SUCCESS) {
		return kr == kIOReturnNotPermitted ? EPERM : EIO;
	}
	return (int)err;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	io_connect_t conn = conn_for_fd(fd);
	if (conn == MACH_PORT_NULL) {
		errno = EBADF;
		return -1;
	}
	int e;
	do {
		e = transport_ioctl(conn, sel_base[fd], request, arg, IOCPARM_LEN(request));
	} while (e == EINTR);
	if (e != 0) {
		errno = e;
		return -1;
	}
	if (DRM_NR(request) == DRM_NR(DRM_IOCTL_GEM_CLOSE)) {
		uint32_t handle = ((struct drm_gem_close *)arg)->handle;
		pthread_mutex_lock(&maps_lock);
		for (size_t i = 0; i < n_maps; i++) {
			if (maps[i].conn == conn && maps[i].handle == handle) {
				maps[i] = maps[--n_maps];
				break;
			}
		}
		pthread_mutex_unlock(&maps_lock);
	}
	return 0;
}

void *
husky_drm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	io_connect_t conn = conn_for_fd(fd);
	if (conn == MACH_PORT_NULL) {
		return mmap(addr, len, prot, flags, fd, offset);
	}

	if ((uint64_t)offset >= USER_MMIO_OFFSET) {
		/* panthor_kmod reads LATEST_FLUSH_ID to skip redundant cache flushes; 0 makes every submit flush. */
		return mmap(NULL, len, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
	}

	if (addr != NULL || (flags & MAP_FIXED)) {
		errno = ENOTSUP;
		return MAP_FAILED;
	}

	const uint32_t handle = (uint32_t)((uint64_t)offset >> 32);
	void *ret = MAP_FAILED;
	pthread_mutex_lock(&maps_lock);
	for (size_t i = 0; i < n_maps; i++) {
		if (maps[i].conn == conn && maps[i].handle == handle) {
			maps[i].refs++;
			ret = (void *)(uintptr_t)maps[i].addr;
			goto out;
		}
	}
	uint64_t in = handle, out[2] = { 0, 0 };
	uint32_t nout = 2;
	kern_return_t kr = IOConnectCallScalarMethod(conn, sel_base[fd] + HUSKY_PNT_SEL_MMAP, &in, 1, out, &nout);
	if (kr != KERN_SUCCESS || out[0] == 0) {
		errno = kr == kIOReturnNotPermitted ? EPERM : ENOMEM;
		goto out;
	}
	if (n_maps == cap_maps) {
		size_t ncap = cap_maps ? cap_maps * 2 : 256;
		struct bo_map *nm = realloc(maps, ncap * sizeof(*nm));
		if (nm == NULL) {
			IOConnectCallScalarMethod(conn, sel_base[fd] + HUSKY_PNT_SEL_MUNMAP, out, 1, NULL, NULL);
			errno = ENOMEM;
			goto out;
		}
		maps = nm;
		cap_maps = ncap;
	}
	maps[n_maps++] = (struct bo_map){ conn, sel_base[fd], handle, 1, out[0], out[1] };
	ret = (void *)(uintptr_t)out[0];
out:
	pthread_mutex_unlock(&maps_lock);
	return ret;
}

int
husky_drm_munmap(void *addr, size_t len)
{
	int found = 0, last = 0;
	io_connect_t conn = MACH_PORT_NULL;
	uint32_t base = 0;
	pthread_mutex_lock(&maps_lock);
	for (size_t i = 0; i < n_maps; i++) {
		if (maps[i].addr == (uint64_t)(uintptr_t)addr) {
			found = 1;
			conn = maps[i].conn;
			base = maps[i].base;
			if (--maps[i].refs == 0) {
				maps[i] = maps[--n_maps];
				last = 1;
			}
			break;
		}
	}
	pthread_mutex_unlock(&maps_lock);
	if (!found) {
		return munmap(addr, len);
	}
	if (last) {
		uint64_t in = (uint64_t)(uintptr_t)addr;
		if (IOConnectCallScalarMethod(conn, base + HUSKY_PNT_SEL_MUNMAP, &in, 1, NULL, NULL) != KERN_SUCCESS) {
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

drmVersionPtr
drmGetVersion(int fd)
{
	if (!husky_drm_is_fd(fd)) {
		errno = EBADF;
		return NULL;
	}
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	/* panthor uAPI revision the kernel implements (MMU_INFO is 1.9). */
	v->version_major = 1;
	v->version_minor = 9;
	v->name = strdup("panthor");
	v->name_len = (int)strlen(v->name);
	v->date = strdup("");
	v->date_len = 0;
	v->desc = strdup("husky panthor over IOKit");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v == NULL) {
		return;
	}
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

void
drmFree(void *p)
{
	free(p);
}

/* The kernel has no GET_CAP; these are what its syncobj implementation provides. */
int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	if (!husky_drm_is_fd(fd)) {
		errno = EBADF;
		return -1;
	}
	switch (capability) {
	case DRM_CAP_SYNCOBJ:
	case DRM_CAP_SYNCOBJ_TIMELINE:
		*value = 1;
		return 0;
	case DRM_CAP_PRIME:
		*value = 0;
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd; (void)flags; (void)device;
	errno = ENOENT;
	return -ENOENT;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	(void)device;
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a == b;
}

/* No DRM nodes exist; Mesa's generic Vulkan runtime still references the enumeration entry points. */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags; (void)devices; (void)max_devices;
	return 0;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices; (void)count;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;
	return -1;
}

/* IOSurface "dma-buf" tokens: placeholder fds remembered with the surface they stand for. */
static struct {
	pthread_mutex_t lock;
	uint32_t surface_id[MAX_FDS], plane[MAX_FDS];
	uint64_t size[MAX_FDS];
	uint8_t used[MAX_FDS];
} tokens = { .lock = PTHREAD_MUTEX_INITIALIZER };

int
husky_drm_surface_fd(int drm_fd, uint32_t surface_id, uint32_t plane)
{
	if (!husky_drm_is_fd(drm_fd)) {
		errno = EBADF;
		return -1;
	}
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0 || fd >= MAX_FDS) {
		if (fd >= 0) {
			close(fd);
			errno = EMFILE;
		}
		return -1;
	}
	pthread_mutex_lock(&tokens.lock);
	tokens.used[fd] = 1;
	tokens.surface_id[fd] = surface_id;
	tokens.plane[fd] = plane;
	tokens.size[fd] = 0;
	pthread_mutex_unlock(&tokens.lock);
	return fd;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	if (prime_fd < 0 || prime_fd >= MAX_FDS) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&tokens.lock);
	int used = tokens.used[prime_fd];
	struct drm_husky_import args = { .surface_id = tokens.surface_id[prime_fd], .plane = tokens.plane[prime_fd] };
	pthread_mutex_unlock(&tokens.lock);
	if (!used) {
		errno = EINVAL;
		return -1;
	}
	if (drmIoctl(fd, HUSKY_IMPORT_REQUEST, &args)) {
		return -1;
	}
	pthread_mutex_lock(&tokens.lock);
	tokens.size[prime_fd] = args.size;
	pthread_mutex_unlock(&tokens.lock);
	*handle = args.handle;
	return 0;
}

/* lseek(fd, 0, SEEK_END) of a dma-buf: the size the import ioctl reported. */
off_t
husky_drm_dmabuf_size(int prime_fd)
{
	off_t size = -1;
	if (prime_fd >= 0 && prime_fd < MAX_FDS) {
		pthread_mutex_lock(&tokens.lock);
		if (tokens.used[prime_fd]) {
			size = (off_t)tokens.size[prime_fd];
		}
		pthread_mutex_unlock(&tokens.lock);
	}
	return size;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close args;
	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create args;
	memclear(args);
	args.flags = flags;
	int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);
	if (ret) {
		return ret;
	}
	*handle = args.handle;
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy args;
	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd; (void)handle; (void)obj_fd;
	errno = ENOSYS;
	return -1;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd; (void)obj_fd; (void)handle;
	errno = ENOSYS;
	return -1;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd; (void)handle; (void)sync_file_fd;
	errno = ENOSYS;
	return -1;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd; (void)handle; (void)sync_file_fd;
	errno = ENOSYS;
	return -1;
}

/* Linux userspace passes an absolute CLOCK_MONOTONIC deadline; the kernel here takes nanoseconds from now. */
static int64_t
relative_timeout(int64_t abs_nsec)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
	return abs_nsec > now ? abs_nsec - now : 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	struct drm_syncobj_wait args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.timeout_nsec = relative_timeout(timeout_nsec);
	args.count_handles = num_handles;
	args.flags = flags;
	int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
	if (ret < 0) {
		return -errno;
	}
	if (first_signaled) {
		*first_signaled = args.first_signaled;
	}
	return ret;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.timeout_nsec = relative_timeout(timeout_nsec);
	args.count_handles = num_handles;
	args.flags = flags;
	int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
	if (ret < 0) {
		return -errno;
	}
	if (first_signaled) {
		*first_signaled = args.first_signaled;
	}
	return ret;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	struct drm_syncobj_timeline_array args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	struct drm_syncobj_timeline_array args;
	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	struct drm_syncobj_transfer args;
	memclear(args);
	args.src_handle = src_handle;
	args.dst_handle = dst_handle;
	args.src_point = src_point;
	args.dst_point = dst_point;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}
