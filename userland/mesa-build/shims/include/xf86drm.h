/* SPDX-License-Identifier: MIT
 * Shim for <xf86drm.h>: iOS has no DRM.
 *
 * panfrost's pan_kmod.h (reached through pan_desc.h / pan_image.h / pan_props.h) includes
 * <xf86drm.h> only for the drmIoctl() used by the pan_kmod_ioctl() macro and for the UAPI types
 * in drm-uapi/drm.h (spec 06 s8 item 4: M:lib/kmod/pan_kmod.h:25-27,625-631). We never compile
 * pan_kmod.c/panthor_kmod.c/panfrost_kmod.c, so nothing calls this; it exists so the header
 * parses. Any accidental call fails loudly with ENOSYS instead of linking against libdrm.
 *
 * ORIGINAL file (no libdrm code copied).
 */
#ifndef MALI_MESA_SHIM_XF86DRM_H
#define MALI_MESA_SHIM_XF86DRM_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int
drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   (void)request;
   (void)arg;
   errno = ENOSYS;
   return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* MALI_MESA_SHIM_XF86DRM_H */
