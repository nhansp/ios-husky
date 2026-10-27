/* SPDX-License-Identifier: MIT
 * ABI assertions for Mali Mesa panfrost libraries.
 *
 * Verifies HAVE_FUNC_ATTRIBUTE_PACKED is defined and struct pan_varying_layout
 * has exact size 152 bytes with correct field offsets.
 */
#ifndef MALI_MESA_ABI_CHECK_H
#define MALI_MESA_ABI_CHECK_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#ifndef HAVE_FUNC_ATTRIBUTE_PACKED
#error "ABI violation: HAVE_FUNC_ATTRIBUTE_PACKED must be defined. Struct layout will mismatch!"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "panfrost/compiler/pan_compiler.h"

#if defined(__cplusplus)
static_assert(sizeof(struct pan_varying_slot) == 4, "sizeof(struct pan_varying_slot) != 4");
static_assert(sizeof(enum pan_varying_knowledge) == 1, "sizeof(enum pan_varying_knowledge) != 1 (check HAVE_FUNC_ATTRIBUTE_PACKED)");
static_assert(sizeof(struct pan_varying_layout) == 152, "ABI mismatch: struct pan_varying_layout size must be exactly 152 bytes");
static_assert(offsetof(struct pan_varying_layout, count) == 0, "pan_varying_layout.count offset");
static_assert(offsetof(struct pan_varying_layout, known) == 1, "pan_varying_layout.known offset");
static_assert(offsetof(struct pan_varying_layout, generic_size_B) == 2, "pan_varying_layout.generic_size_B offset");
static_assert(offsetof(struct pan_varying_layout, slots) == 4, "pan_varying_layout.slots offset");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct pan_varying_slot) == 4, "sizeof(struct pan_varying_slot) != 4");
_Static_assert(sizeof(enum pan_varying_knowledge) == 1, "sizeof(enum pan_varying_knowledge) != 1 (check HAVE_FUNC_ATTRIBUTE_PACKED)");
_Static_assert(sizeof(struct pan_varying_layout) == 152, "ABI mismatch: struct pan_varying_layout size must be exactly 152 bytes");
_Static_assert(offsetof(struct pan_varying_layout, count) == 0, "pan_varying_layout.count offset");
_Static_assert(offsetof(struct pan_varying_layout, known) == 1, "pan_varying_layout.known offset");
_Static_assert(offsetof(struct pan_varying_layout, generic_size_B) == 2, "pan_varying_layout.generic_size_B offset");
_Static_assert(offsetof(struct pan_varying_layout, slots) == 4, "pan_varying_layout.slots offset");
#endif

#ifdef __cplusplus
}
#endif

#endif /* MALI_MESA_ABI_CHECK_H */
