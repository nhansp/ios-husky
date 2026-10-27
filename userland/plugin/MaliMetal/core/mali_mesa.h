/*
 * mali_mesa.h - the Mesa headers the core uses, in the one order that works
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_mesa.h
 *
 * INCLUDE THIS FIRST in every core .c file that touches Mesa, then "mali_core.h".
 *
 * Why the order matters: the contract header mali_backend_ops.h defines the
 * macros MALI_CS_MOVE48, MALI_CS_MOVE32, MALI_CS_WAIT, MALI_CS_STORE_MULTIPLE,
 * MALI_CS_FLUSH_CACHE2, MALI_CS_SYNC_ADD64, MALI_CS_CALL and MALI_CS_ERROR_BARRIER
 * (the kernel's hand-encoded instruction words). Mesa's generated v11_pack.h
 * declares structs and enumerators with the same names (struct MALI_CS_CALL,
 * ...), so once the contract header has been seen, `struct MALI_CS_ERROR_BARRIER`
 * no longer parses. Everything below is declared before the contract header and
 * the cs_builder helpers are static inline functions already compiled, so the
 * only rule left is: after mali_core.h, never expand a Mesa macro that pastes
 * one of those eight CS names (cs_emit(b, ERROR_BARRIER..) etc.); call the
 * cs_* functions instead. Reported as an open question: the contract should
 * prefix those macros (MALI_KCS_*), they are kernel-private encodings.
 */
#ifndef MALI_MESA_H
#define MALI_MESA_H

#ifndef PAN_ARCH
#define PAN_ARCH 11
#endif

#include "genxml/gen_macros.h"
#include "genxml/cs_builder.h"
#include "pan_blend.h"
#include "pan_desc.h"
#include "pan_earlyzs.h"
#include "pan_fb.h"
#include "pan_format.h"
#include "pan_image.h"
#include "pan_layout.h"
#include "pan_mod.h"
#include "pan_props.h"
#include "pan_texture.h"
#include "util/format/u_format.h"
#include "drm-uapi/drm_fourcc.h"

#endif
