/*
 * vh_fau.c - FAU sysval layout tables and slot remapping (spec 07 6.11).
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/src/vh_fau.c
 * ORIGINAL code, Mesa-free. See vh_fau.h for the model and citations.
 */
#include <stddef.h>
#include <string.h>
#include "vh_fau.h"

#define MEMBER(T, id, field, count)                                          \
    { (id), (count), (uint16_t)offsetof(T, field),                           \
      (uint16_t)sizeof(((T *)0)->field),                                     \
      (uint16_t)(sizeof(((T *)0)->field) / (count)) }

/* Member tables straight from the contract structs (air2vh_api.h 3.0), so a
 * layout change there is picked up by recompiling. */
static const vh_sysval_member k_gfx[] = {
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_BLEND_CONSTANTS, blend.constants, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_VIEWPORT_SCALE, viewport_scale, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_VIEWPORT_OFFSET, viewport_offset, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_FIRST_VERTEX, first_vertex, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_BASE_INSTANCE, base_instance, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_NOPERSPECTIVE_VARYINGS, noperspective_varyings, 1),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_BLEND_DESCS, blend_descs, 8),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_RT_CONV, rt_conv, 8),
    MEMBER(air2vh_sysvals_gfx, AIR2VH_SV_GFX_BUFFER_VA, buffer_va, 31),
};
static const vh_sysval_member k_cs[] = {
    MEMBER(air2vh_sysvals_cs, AIR2VH_SV_NUM_WORK_GROUPS, num_work_groups, 1),
    MEMBER(air2vh_sysvals_cs, AIR2VH_SV_LOCAL_GROUP_SIZE, local_group_size, 1),
    MEMBER(air2vh_sysvals_cs, AIR2VH_SV_THREADS_PER_GRID, threads_per_grid, 1),
    MEMBER(air2vh_sysvals_cs, AIR2VH_SV_TG_OFFSET, tg_offset, 31),
    MEMBER(air2vh_sysvals_cs, AIR2VH_SV_CS_BUFFER_VA, buffer_va, 31),
};

/* Blend constants must sit at struct offset 0 so they land in FAU words 0..3
 * whatever else the FS uses (C11). */
_Static_assert(offsetof(air2vh_sysvals_gfx, blend) == 0, "blend constants first");
_Static_assert(sizeof(((air2vh_sysvals_gfx *)0)->blend.constants) == 16, "4 words");
_Static_assert(offsetof(air2vh_sysvals_gfx, buffer_va) % 8 == 0, "64-bit VAs 8-aligned");
_Static_assert(offsetof(air2vh_sysvals_gfx, blend_descs) % 8 == 0, "64-bit blend descs");
_Static_assert(offsetof(air2vh_sysvals_cs, buffer_va) % 8 == 0, "64-bit VAs 8-aligned");
_Static_assert(sizeof(air2vh_sysvals_gfx) <= 64 * 8, "fits the 64-bit used-word mask");
_Static_assert(sizeof(air2vh_sysvals_cs) <= 64 * 8, "fits the 64-bit used-word mask");

static const vh_sysval_member *table_of(enum vh_sv_struct st, unsigned *n)
{
    if (st == VH_SV_CS) {
        *n = sizeof k_cs / sizeof k_cs[0];
        return k_cs;
    }
    *n = sizeof k_gfx / sizeof k_gfx[0];
    return k_gfx;
}

uint32_t vh_sysval_struct_size(enum vh_sv_struct st)
{
    return st == VH_SV_CS ? (uint32_t)sizeof(air2vh_sysvals_cs)
                          : (uint32_t)sizeof(air2vh_sysvals_gfx);
}

int vh_sysval_offset(enum vh_sv_struct st, uint32_t id, uint32_t index,
                     uint32_t *offset, uint32_t *elem_size)
{
    unsigned n;
    const vh_sysval_member *t = table_of(st, &n);
    for (unsigned i = 0; i < n; i++) {
        if (t[i].id != id)
            continue;
        if (index >= t[i].count)
            return -1;
        *offset = t[i].offset + index * t[i].elem_size;
        if (elem_size)
            *elem_size = t[i].elem_size;
        return 0;
    }
    return -1;
}

uint32_t vh_sysval_at(enum vh_sv_struct st, uint32_t off)
{
    unsigned n;
    const vh_sysval_member *t = table_of(st, &n);
    for (unsigned i = 0; i < n; i++)
        if (off >= t[i].offset && off < (uint32_t)t[i].offset + t[i].size)
            return t[i].id;
    return AIR2VH_SV_NONE;
}

void vh_fau_init(vh_fau_plan *p, enum vh_sv_struct st, int is_fragment)
{
    memset(p, 0, sizeof *p);
    p->st = st;
    if (is_fragment && st == VH_SV_GFX)
        vh_fau_mark(p, (uint32_t)offsetof(air2vh_sysvals_gfx, blend.constants), 16);
}

void vh_fau_mark(vh_fau_plan *p, uint32_t off, uint32_t size)
{
    if (!size)
        return;
    if (off + size > vh_sysval_struct_size(p->st)) {
        p->overflow = 1;
        return;
    }
    for (uint32_t w = off / 8; w <= (off + size - 1) / 8; w++)
        p->used |= 1ull << w;
}

unsigned vh_fau_sysval_slots(const vh_fau_plan *p)
{
    return (unsigned)__builtin_popcountll(p->used);
}

uint32_t vh_fau_remap(const vh_fau_plan *p, uint32_t off)
{
    uint32_t w = off / 8;
    uint64_t below = w ? (p->used & ((1ull << w) - 1ull)) : 0;
    return 8u * (uint32_t)__builtin_popcountll(below) + (off % 8u);
}

int vh_fau_emit(const vh_fau_plan *p, air2vh_program *prog, uint32_t total_words,
                const uint32_t *imm_word, const uint32_t *imm_value, uint32_t n_imm)
{
    unsigned sv = vh_fau_sysval_slots(p);
    uint32_t slots = (total_words + 1u) / 2u;
    if (slots < sv)
        slots = sv;
    if (p->overflow || sv > VH_FAU_SYSVAL_SLOT_BUDGET || slots > AIR2VH_FAU_SLOTS ||
        n_imm > AIR2VH_FAU_WORDS)
        return -1;

    memset(&prog->fau, 0, sizeof prog->fau);
    prog->fau.slots = slots;
    prog->fau.reserved_words = 2u * sv;
    unsigned k = 0;
    for (unsigned w = 0; w < 64; w++) {
        if (!(p->used & (1ull << w)))
            continue;
        prog->fau.src[k].sysval = (uint8_t)vh_sysval_at(p->st, 8u * w);
        prog->fau.src[k].byte_offset = 8u * w;
        k++;
    }
    prog->fau.n_src = k;
    for (uint32_t i = 0; i < n_imm; i++) {
        if (imm_word[i] < prog->fau.reserved_words || imm_word[i] >= 2u * slots)
            return -1;                 /* immediates live after the sysval range */
        prog->fau.imm[i].word = imm_word[i];
        prog->fau.imm[i].value = imm_value[i];
    }
    prog->fau.n_imm = n_imm;
    return 0;
}
