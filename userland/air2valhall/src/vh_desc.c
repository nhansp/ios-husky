/*
 * vh_desc.c - Mesa-free VH-ABI 1 descriptor packers, SPD packer, FAU sysval
 *             names, program-extension lookup and tier-A varying linking.
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/src/vh_desc.c
 * Declared in include/air2vh_builder.h and include/air2vh_ext.h.
 *
 * ORIGINAL code. Every bit position is from Mesa's v11 descriptor XML
 * (gpuref/mesa/src/panfrost/genxml/v11.xml, cited "v11.xml:<line>"); field
 * usage follows panvk (MIT, cited "MP/vulkan/<file>:<line>"). The genxml
 * packers are not used here on purpose: the plug-in's command builder must be
 * able to include air2vh_builder.h without Mesa. tests/test_vh_standalone.c
 * checks the packed words against hand-computed values from the XML.
 */
#include <stdint.h>
#include <string.h>
#include "air2vh_api.h"
#include "air2vh_builder.h"
#include "air2vh_ext.h"

/* Set `size` bits at bit `start` of a little-endian dword array. */
static void put_bits(uint32_t *w, unsigned start, unsigned size, uint64_t v)
{
    for (unsigned i = 0; i < size; i++) {
        unsigned b = start + i;
        uint32_t m = 1u << (b & 31);
        if ((v >> i) & 1)
            w[b >> 5] |= m;
        else
            w[b >> 5] &= ~m;
    }
}

/* ---------------------------------------------------------------- sysvals */

const char *air2vh_sysval_name(uint32_t s)
{
    static const char *const n[AIR2VH_SV_COUNT] = {
        [AIR2VH_SV_NONE] = "none",
        [AIR2VH_SV_BLEND_CONSTANTS] = "blend.constants",
        [AIR2VH_SV_VIEWPORT_SCALE] = "viewport_scale",
        [AIR2VH_SV_VIEWPORT_OFFSET] = "viewport_offset",
        [AIR2VH_SV_FIRST_VERTEX] = "first_vertex",
        [AIR2VH_SV_BASE_INSTANCE] = "base_instance",
        [AIR2VH_SV_NOPERSPECTIVE_VARYINGS] = "noperspective_varyings",
        [AIR2VH_SV_BLEND_DESCS] = "blend_descs",
        [AIR2VH_SV_RT_CONV] = "rt_conv",
        [AIR2VH_SV_GFX_BUFFER_VA] = "buffer_va",
        [AIR2VH_SV_NUM_WORK_GROUPS] = "num_work_groups",
        [AIR2VH_SV_LOCAL_GROUP_SIZE] = "local_group_size",
        [AIR2VH_SV_THREADS_PER_GRID] = "threads_per_grid",
        [AIR2VH_SV_TG_OFFSET] = "tg_offset",
        [AIR2VH_SV_CS_BUFFER_VA] = "buffer_va",
    };
    return s < AIR2VH_SV_COUNT && n[s] ? n[s] : "invalid";
}

/* ------------------------------------------------------------ descriptors */

void air2vh_desc_null(uint32_t d[8])
{
    memset(d, 0, 32);                       /* Type Null = 0 (v11.xml:1180-1182) */
}

void air2vh_desc_buffer(uint32_t d[8], uint64_t va, uint64_t size)
{
    memset(d, 0, 32);
    put_bits(d, 0, 4, AIR2VH_MALI_DESC_BUFFER); /* Type (v11.xml:1294)          */
    put_bits(d, 4, 4, 1);                       /* Buffer type Simple (:1295, :53-58) */
    d[1] = (uint32_t)size;                      /* Size  @1:0  (:1296)          */
    d[2] = (uint32_t)va;                        /* Address @2:0, 64 bits (:1297)*/
    d[3] = (uint32_t)(va >> 32);
    d[5] = (uint32_t)(size >> 32);              /* Size hi @5:0 (:1307)         */
}

void air2vh_desc_ubo(uint32_t d[8], uint64_t va, uint64_t bound_length)
{
    if (!va || !bound_length) {
        air2vh_desc_null(d);
        return;
    }
    air2vh_desc_buffer(d, va, (bound_length + 15u) & ~(uint64_t)15u);
}

void air2vh_desc_attribute(uint32_t d[8], const air2vh_attr_desc *a)
{
    memset(d, 0, 32);
    put_bits(d, 0, 4, AIR2VH_MALI_DESC_ATTRIBUTE); /* Type (v11.xml:1185)       */
    put_bits(d, 4, 4, a->attribute_type);          /* Attribute type @0:4 (:1186) */
    put_bits(d, 8, 1, a->offset_enable ? 1 : 0);   /* Offset enable @8 (:1187)   */
    put_bits(d, 10, 22, a->format_hw);             /* Format @0:10, 22 bits (:1188) */
    put_bits(d, 32 + 0, 6, a->table);              /* Table @1:0 (:1189)         */
    put_bits(d, 32 + 6, 2, a->per_instance ? 1 : 0); /* Frequency @1:6 (:1190)  */
    put_bits(d, 32 + 24, 5, a->divisor_r);         /* Divisor R @1:24 (:1191)    */
    put_bits(d, 32 + 29, 1, a->divisor_e);         /* Divisor E @1:29 (:1192)    */
    d[2] = (uint32_t)a->offset;                    /* Offset @2:0 (:1193)        */
    d[3] = a->buffer_index;                        /* Buffer index @3:0 (:1194)  */
    d[4] = a->stride;                              /* Stride @4:0 (:1195)        */
    d[5] = a->divisor_d;                           /* Divisor D @5:0 (:1201)     */
}

void air2vh_desc_attribute_unused(uint32_t d[8])
{
    air2vh_attr_desc a;
    memset(&a, 0, sizeof a);
    a.table = 17;                                  /* invalid table -> OOB reads */
    a.format_hw = (199u << 12) | 0u;               /* MALI_R16F << 12 | RGBA (v11.xml:248, :292) */
    air2vh_desc_attribute(d, &a);
    /* panvk packs this with attribute_type left at its default 0
     * (MP/vulkan/csf/panvk_vX_cmd_draw.c:358-364); we do the same. */
}

void air2vh_desc_varying(uint32_t d[8], uint32_t format_hw, uint32_t vs_slot_offset,
                         uint32_t generic_size_B)
{
    air2vh_attr_desc a;
    memset(&a, 0, sizeof a);
    a.attribute_type = AIR2VH_ATTR_VERTEX_PACKET;
    a.offset_enable = 0;
    a.format_hw = format_hw;
    a.table = 61;                                  /* hardware varying buffer */
    a.per_instance = 0;
    a.offset = (int32_t)(1024u + vs_slot_offset);
    a.buffer_index = 0;                            /* < v12 (panvk_vX_cmd_draw.c:503) */
    air2vh_desc_attribute(d, &a);
    /* Vertex-packet form of word 4: Attribute stride @4:0, Packet stride
     * @4:16 (v11.xml:1198-1199). */
    d[4] = (generic_size_B & 0xffffu) | (((generic_size_B + 16u) & 0xffffu) << 16);
}

void air2vh_desc_dummy_sampler(uint32_t d[8])
{
    memset(d, 0, 32);
    put_bits(d, 0, 4, AIR2VH_MALI_DESC_SAMPLER);  /* Type (v11.xml:1394)            */
    put_bits(d, 8, 4, 9);                         /* Wrap R default Clamp to Edge (:1396) */
    put_bits(d, 12, 4, 9);                        /* Wrap T                          */
    put_bits(d, 16, 4, 9);                        /* Wrap S                          */
    put_bits(d, 21, 1, 1);                        /* Round to nearest even default 1 (:1399) */
    put_bits(d, 23, 1, 1);                        /* Seamless cube map default 1 (:1402) */
    put_bits(d, 25, 1, 1);                        /* Normalized coordinates default 1 (:1404) */
    put_bits(d, 26, 1, 0);                        /* Clamp integer array indices = false
                                                     (panvk_vX_cmd_draw.c:368-370)   */
}

void air2vh_resource_entry(uint32_t e[4], uint64_t va, uint64_t size, int contains)
{
    memset(e, 0, 16);
    if (!va || !size)
        return;                                    /* all-zero entry (panvk_vX_cmd_desc_state.c:300-305) */
    put_bits(e, 0, 56, va & 0x00ffffffffffffffULL); /* Address @0:0, 56 bits (v11.xml:2366) */
    put_bits(e, 32 + 24, 1, contains ? 1 : 0);      /* Contains descriptors @1:24 (:2367)   */
    e[2] = (uint32_t)size;                          /* Size @2:0, 64 bits (:2368)           */
    e[3] = (uint32_t)(size >> 32);
}

int air2vh_srt_fill(uint32_t srt[AIR2VH_SRT_ENTRY_COUNT][4], const air2vh_program *p,
                    const uint64_t table_va[AIR2VH_T_COUNT],
                    const uint32_t count[AIR2VH_T_COUNT])
{
    memset(srt, 0, sizeof(uint32_t) * 4 * AIR2VH_SRT_ENTRY_COUNT);
    for (unsigned t = 0; t < AIR2VH_T_COUNT; t++) {
        if (p && count[t] < p->table_count[t])
            return -1;
        if (count[t] && table_va[t]) {
            if (table_va[t] & (AIR2VH_DESCRIPTOR_BYTES - 1))
                return -1;                         /* descriptors are 32-byte aligned */
            air2vh_resource_entry(srt[t], table_va[t],
                                  (uint64_t)count[t] * AIR2VH_DESCRIPTOR_BYTES, 1);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------- SPD */

int air2vh_spd_pack(uint32_t d[8], const air2vh_program *p, unsigned i, uint64_t code_va)
{
    if (!p || i >= p->spd_count || i >= 3)
        return -1;
    /* the inline constant pool is PC-relative with a 32-bit offset
     * (MP/vulkan/panvk_vX_shader.c:1208-1212, claim C7) */
    uint64_t end = code_va + (p->code_size ? p->code_size - 1 : 0);
    if ((code_va >> AIR2VH_EXEC_WINDOW_BITS) != (end >> AIR2VH_EXEC_WINDOW_BITS))
        return -1;
    if (code_va & (AIR2VH_CODE_ALIGN - 1))
        return -1;

    unsigned stage = p->stage == AIR2VH_STAGE_VERTEX ? 3u      /* Shader stage Vertex  (v11.xml:39) */
                   : p->stage == AIR2VH_STAGE_FRAGMENT ? 2u    /* Fragment (:38) */
                   : 1u;                                       /* Compute  (:37) */
    unsigned ftz = p->ftz_fp32 ? (p->ftz_fp16 ? 2u : 1u) : 0u; /* Always / DX11 / Preserve
                                                                  (v11.xml:1743-1748; panvk_vX_shader.c:1168-1183) */
    unsigned regalloc = p->work_reg_count[i] <= 32 ? 2u : 0u;  /* 32 / 64 per thread (v11.xml:1738-1741) */

    memset(d, 0, 32);
    put_bits(d, 0, 4, AIR2VH_MALI_DESC_SHADER);  /* Type (v11.xml:1776)              */
    put_bits(d, 4, 4, stage);                    /* Stage @0:4 (:1777)               */
    if (p->stage == AIR2VH_STAGE_FRAGMENT)
        put_bits(d, 8, 1, 1);                    /* coverage bitmask GL (:1778, :1765) */
    else if (p->stage == AIR2VH_STAGE_VERTEX)    /* warp limit (:1779, :1768-1773):    */
        put_bits(d, 8, 2, i == AIR2VH_VS_SPD_VARYING ? 0u /* Full */ : 1u /* Half */);
    put_bits(d, 17, 2, ftz);                     /* Flush to zero mode @0:17 (:1781)  */
    if (p->stage == AIR2VH_STAGE_FRAGMENT)
        put_bits(d, 28, 1, p->requires_helper_threads ? 1 : 0); /* @0:28 (:1783)       */
    put_bits(d, 30, 2, regalloc);                /* Register allocation @0:30 (:1785) */
    d[1] = (uint32_t)((p->preload[i] >> 48) & 0xffffu); /* Preload.R48-R63 @1:0 (:1786, :1751) */
    uint64_t bin = code_va + p->entry_offset[i];
    d[2] = (uint32_t)bin;                        /* Binary @2:0 (:1787)              */
    d[3] = (uint32_t)(bin >> 32);
    return 0;
}

/* -------------------------------------------------------- program ext */

const air2vh_program_ext *air2vh_program_get_ext(const air2vh_program *p)
{
    if (!p || p->struct_size != sizeof(air2vh_program) || (p->struct_size & 7))
        return NULL;
    const air2vh_program_ext *e =
        (const air2vh_program_ext *)((const uint8_t *)p + p->struct_size);
    if (e->magic != AIR2VH_EXT_MAGIC || e->version != AIR2VH_EXT_VERSION ||
        e->size != sizeof(air2vh_program_ext))
        return NULL;
    return e;
}

/* ------------------------------------------------------- varying link */

/* nir_alu_type masks (MN/nir_defines.h:266-267). */
#define VH_ALU_SIZE_MASK 0x79u
#define VH_ALU_BASE_MASK 0x86u
#define VH_SECTION_GENERIC 3u   /* PAN_VARYING_SECTION_GENERIC (pan_compiler.h:224) */

int air2vh_link_varyings(const air2vh_program *vs, const air2vh_program *fs,
                         air2vh_varying_link *out, uint32_t max, uint32_t *generic_size_B)
{
    const air2vh_program_ext *ve = air2vh_program_get_ext(vs);
    const air2vh_program_ext *fe = air2vh_program_get_ext(fs);
    if (!ve || !fe || vs->stage != AIR2VH_STAGE_VERTEX || fs->stage != AIR2VH_STAGE_FRAGMENT ||
        (max && !out))
        return -1;
    if (generic_size_B)
        *generic_size_B = ve->generic_size_B;

    uint32_t total = 0;
    for (uint32_t i = 0; i < fe->n_varyings && i < AIR2VH_EXT_MAX_VARYINGS; i++) {
        const air2vh_varying_slot *f = &fe->varyings[i];
        if (!f->alu_type || f->section != VH_SECTION_GENERIC)
            continue;                         /* empty or special (panvk_vX_cmd_draw.c:476-478) */
        total++;
    }

    if (!out || max == 0)
        return (int)total;

    if (max < total)
        return -1;                            /* capacity too small; report error rather than silent truncation */

    uint32_t n = 0;
    for (uint32_t i = 0; i < fe->n_varyings && i < AIR2VH_EXT_MAX_VARYINGS; i++) {
        const air2vh_varying_slot *f = &fe->varyings[i];
        if (!f->alu_type || f->section != VH_SECTION_GENERIC)
            continue;                         /* empty or special (panvk_vX_cmd_draw.c:476-478) */
        air2vh_varying_link *l = &out[n++];
        memset(l, 0, sizeof *l);
        l->fs_index = i;
        for (uint32_t j = 0; j < ve->n_varyings && j < AIR2VH_EXT_MAX_VARYINGS; j++) {
            const air2vh_varying_slot *v = &ve->varyings[j];
            if (!v->alu_type || v->section != VH_SECTION_GENERIC || !f->name[0] ||
                strncmp(v->name, f->name, AIR2VH_EXT_NAME_LEN) != 0)
                continue;
            l->matched = 1;
            l->vs_offset = v->offset >= 0 ? (uint32_t)v->offset : 0;
            l->alu_type = (uint8_t)((f->alu_type & VH_ALU_BASE_MASK) | (v->alu_type & VH_ALU_SIZE_MASK));
            l->ncomps = v->ncomps;
            break;
        }
    }
    return (int)n;
}
