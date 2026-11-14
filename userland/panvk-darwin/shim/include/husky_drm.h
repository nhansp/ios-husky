/* libdrm replacement for the husky panthor IOKit transport; see husky_drm.c. */
#ifndef HUSKY_DRM_H
#define HUSKY_DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the MaliAccelerator user client. The returned fd is a placeholder descriptor keying the connection. */
int husky_drm_open(void);
void husky_drm_close(int fd);
int husky_drm_is_fd(int fd);

/* A token fd standing for plane `plane` of an IOSurface; drmPrimeFDToHandle on it imports the surface. */
int husky_drm_surface_fd(int drm_fd, uint32_t surface_id, uint32_t plane);
off_t husky_drm_dmabuf_size(int prime_fd);

/* mmap/munmap drop-ins: BO offsets (handle << 32) and the flush-id page are served here, anything else falls
 * through to the system calls. */
void *husky_drm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int husky_drm_munmap(void *addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
