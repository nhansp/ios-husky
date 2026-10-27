/*
 * compat/xf86drm.h - stand-in for libdrm's header
 * Intended repo path: plugin/MaliMetal/compat/xf86drm.h
 *
 * Mesa's pan_kmod headers include <xf86drm.h> for drmIoctl(); the plug-in never talks DRM (the kernel
 * side is the IOGPU user client), so the include resolves to this stub and drmIoctl always fails.
 * Only the core's C files see this directory (build.sh / tests/host/build_host.sh -I).
 */
#pragma once
#include <stdint.h>
static inline int drmIoctl(int fd, unsigned long req, void *arg) { (void)fd; (void)req; (void)arg; return -1; }
