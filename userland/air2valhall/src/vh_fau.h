/*
 * vh_fau.h - FAU sysval layout + packing plan (compiler side, spec 07 6.11).
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/src/vh_fau.h
 *
 * Mesa-free so the remap arithmetic is unit-testable on the host
 * (tests/test_vh_standalone.c). The NIR side (src/vh_lower_bindings.cpp) emits
 * load_push_constant(base = VH_SYSVAL_PUSH_BASE, offset = imm(byte offset in
 * the stage's sysval struct)), collects every such load into a vh_fau_plan,
 * and rewrites the offsets with vh_fau_remap() so that the used 8-byte words
 * of the struct become consecutive FAU slots 0..n-1, blend constants first.
 *
 * Model: panvk's collect_push_constant / move_push_constant /
 * shader_remapped_sysval_offset (MP/vulkan/panvk_vX_shader.c:630-790,
 * MP/vulkan/panvk_shader.h:263-310), re-implemented (spec 07 6.11 says this
 * packing is driver code we must own).
 * ORIGINAL code.
 */
#ifndef VH_FAU_H
#define VH_FAU_H

#include <stdint.h>
#include "air2vh_api.h"
#include "air2vh_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Marker base of a not-yet-remapped sysval load. Any value above both struct
 * sizes works; it plays the role of panvk's SYSVALS_PUSH_CONST_BASE
 * (MP/vulkan/panvk_shader.h:243). */
#define VH_SYSVAL_PUSH_BASE 0x1000u

enum vh_sv_struct { VH_SV_GFX = 0, VH_SV_CS = 1 };

typedef struct vh_sysval_member {
    uint8_t  id;          /* enum air2vh_sysval */
    uint8_t  count;       /* array length (1 for scalars/vectors) */
    uint16_t offset;      /* byte offset in the struct */
    uint16_t size;        /* total bytes */
    uint16_t elem_size;   /* bytes per array element (== size for count 1) */
} vh_sysval_member;

/* Byte offset of element `index` of sysval `id` in struct `st`; -1 if the
 * sysval is not in that struct or index is out of range. */
int vh_sysval_offset(enum vh_sv_struct st, uint32_t id, uint32_t index,
                     uint32_t *offset, uint32_t *elem_size);
/* Member containing byte `off` (AIR2VH_SV_NONE for padding). */
uint32_t vh_sysval_at(enum vh_sv_struct st, uint32_t off);
uint32_t vh_sysval_struct_size(enum vh_sv_struct st);

typedef struct vh_fau_plan {
    enum vh_sv_struct st;
    uint64_t used;        /* bit w: 8-byte word w of the struct is read */
    int      overflow;    /* a load fell outside the struct */
} vh_fau_plan;

/* FS: blend constants (words 0..3 = slots 0..1) are always reserved because
 * a blend shader may read them (C11; MP/vulkan/panvk_vX_shader.c:750-761). */
void     vh_fau_init(vh_fau_plan *p, enum vh_sv_struct st, int is_fragment);
void     vh_fau_mark(vh_fau_plan *p, uint32_t byte_off, uint32_t size);
unsigned vh_fau_sysval_slots(const vh_fau_plan *p);
/* Remapped FAU byte offset of struct byte offset `off` (must be marked). */
uint32_t vh_fau_remap(const vh_fau_plan *p, uint32_t off);

/*
 * Fill program->fau after pan_shader_compile:
 *   total_words = info.fau.count (32-bit words incl. reserved + immediates),
 *   imm_word/imm_value = the compiler-promoted constants (pan_fau_foreach_imm).
 * slots = DIV_ROUND_UP(total_words, 2) (MP/vulkan/panvk_vX_shader.c:1104-1107).
 * Returns 0, or -1 if the budget (64 slots) is exceeded.
 */
int vh_fau_emit(const vh_fau_plan *p, air2vh_program *prog, uint32_t total_words,
                const uint32_t *imm_word, const uint32_t *imm_value, uint32_t n_imm);

/* Sysval slot budget [D, spec 07 6.4]: refuse above 64 - 8, leaving at least
 * 8 slots for promoted immediates. */
#define VH_FAU_SYSVAL_SLOT_BUDGET (AIR2VH_FAU_SLOTS - 8u)

#ifdef __cplusplus
}
#endif
#endif /* VH_FAU_H */
