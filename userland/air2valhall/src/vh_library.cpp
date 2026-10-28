/* SPDX-License-Identifier: MIT */
/*
 * vh_library.cpp - libair2valhall C API implementation (spec 07 sections 4, 7, 8).
 *
 * Pipeline (pan_compiler.h:66-76, panvk_vX_shader.c:1480-1620):
 *   1. air_frontend_mem() -> NIR + AirFrontendResult
 *   2. air2vh_prepare_pre_nir() -> Tier-B FS varying remap, nonuniform dispatch guard
 *   3. pan_preprocess_nir(nir, 11)
 *   4. air2vh_lower_bindings() -> SRT handles, FAU sysval markers, inline constant pool
 *   5. nir_assign_io_var_locations + nir_lower_io + pan_nir_lower_mediump_io
 *   6. VS: pan_varying_collect_formats + pan_build_varying_layout_compact
 *   7. pan_postprocess_nir(nir, &inputs, &sinfo)
 *   8. air2vh_lower_sysvals_and_fau() -> FAU slot packing & offset remapping
 *   9. pan_shader_compile(nir, &inputs, &binary, &sinfo)
 *  10. pan_earlyzs_analyze(&sinfo, 11) + populate air2vh_program, air2vh_program_ext, JSON sidecar
 */
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"

#include "air2vh_api.h"
#include "air2vh_ext.h"
#include "air2vh_builder.h"
#include "vh_fau.h"
#include "vh_sampler.h"
#include "vh_lower_bindings.h"
#include "air2ir3.h"

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "util/ralloc.h"
#include "util/format/u_format.h"
extern "C" {
#include "panfrost/model/pan_model.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/lib/pan_earlyzs.h"
#include "panfrost/lib/pan_blend.h"
}

static_assert(sizeof(vh_vs_layout_blob{}.pan_bytes) == sizeof(struct pan_varying_layout),
              "vh_vs_layout_blob.pan_bytes must match struct pan_varying_layout");
static_assert((sizeof(air2vh_program) & 7u) == 0u,
              "sizeof(air2vh_program) must be 8-byte aligned for air2vh_program_get_ext");

struct VhProgramWithExt {
    air2vh_program prog;
    air2vh_program_ext ext;
};
static_assert(offsetof(VhProgramWithExt, ext) == sizeof(air2vh_program),
              "air2vh_program_ext must follow air2vh_program with zero padding");

struct air2vh_compiler {
    const struct pan_model *model = nullptr;
    uint64_t gpu_id = 0;
    uint32_t gpu_prod_id = 0;
    uint32_t core_count = 7;
    uint64_t gpu_features = 0;
    uint64_t shader_present = 0x01110055ull; /* Husky G715 MC7 (max core ID 24) */
    std::mutex mtx;
};

namespace {

static uint32_t
read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t
read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
read_le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t
read_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static bool
is_raw_bitcode(const uint8_t *p, size_t n)
{
    if (n < 4)
        return false;
    return (p[0] == 0xde && p[1] == 0xc0 && p[2] == 0x17 && p[3] == 0x0b) ||
           (p[0] == 'B' && p[1] == 'C' && p[2] == 0xc0 && p[3] == 0xde);
}

/* Extract function bitcode if caller passed a fat .metallib or MTLB slice. */
static bool
extract_bitcode_slice(const void *buf, size_t size, const char *entry,
                      const void **bc_out, size_t *bc_size_out,
                      std::string *err_out)
{
    if (!buf || size < 4) {
        if (err_out) *err_out = "bitcode buffer is null or too small";
        return false;
    }
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    if (is_raw_bitcode(p, size)) {
        *bc_out = buf;
        *bc_size_out = size;
        return true;
    }

    size_t sl_off = 0, sl_size = size;
    if (size >= 8 && read_be32(p) == 0xCAFEBABEu) {
        uint32_t n = read_be32(p + 4);
        if (n == 0 || n > 16 || 8 + 20ull * n > size) {
            if (err_out) *err_out = "invalid fat metallib header";
            return false;
        }
        bool found = false;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = p + 8 + 20 * i;
            uint32_t cpu = read_be32(e);
            uint32_t off = read_be32(e + 8);
            uint32_t sz = read_be32(e + 12);
            if ((uint64_t)off + sz > size)
                continue;
            if (sz >= 4 && memcmp(p + off, "MTLB", 4) == 0) {
                if (!found || cpu == 0x01000017u) {
                    sl_off = off;
                    sl_size = sz;
                    found = true;
                    if (cpu == 0x01000017u)
                        break;
                }
            }
        }
        if (!found) {
            if (err_out) *err_out = "no MTLB slice found in fat container";
            return false;
        }
    }

    const uint8_t *m = p + sl_off;
    if (sl_size < 0x58 || memcmp(m, "MTLB", 4) != 0) {
        if (err_out) *err_out = "input is neither LLVM bitcode nor MTLB metallib";
        return false;
    }

    uint64_t fl_off = read_le64(m + 0x18);
    uint64_t fl_size = read_le64(m + 0x20);
    uint64_t bc_sec_off = read_le64(m + 0x48);
    uint64_t bc_sec_size = read_le64(m + 0x50);
    if (fl_off + fl_size > sl_size || bc_sec_off + bc_sec_size > sl_size || fl_size < 4) {
        if (err_out) *err_out = "MTLB section offsets out of bounds";
        return false;
    }

    uint32_t count = read_le32(m + fl_off);
    uint64_t cur = fl_off + 4;
    for (uint32_t i = 0; i < count && cur + 4 <= fl_off + fl_size; i++) {
        uint32_t esz = read_le32(m + cur);
        if (esz < 8 || cur + esz > fl_off + fl_size)
            break;
        uint64_t q = cur + 4, end = cur + esz;
        std::string fn_name;
        uint64_t fn_bc_off = 0, fn_bc_size = 0;
        while (q + 4 <= end) {
            const uint8_t *tag = m + q;
            q += 4;
            if (memcmp(tag, "ENDT", 4) == 0)
                break;
            if (q + 2 > end)
                break;
            uint16_t len = read_le16(m + q);
            q += 2;
            if (q + len > end)
                break;
            const uint8_t *v = m + q;
            if (memcmp(tag, "NAME", 4) == 0)
                fn_name.assign(reinterpret_cast<const char *>(v),
                               strnlen(reinterpret_cast<const char *>(v), len));
            else if (memcmp(tag, "MDSZ", 4) == 0 && len >= 8)
                fn_bc_size = read_le64(v);
            else if (memcmp(tag, "OFFT", 4) == 0 && len >= 24)
                fn_bc_off = read_le64(v + 16);
            q += len;
        }
        bool match = (!entry || !*entry) ? (i == 0) : (fn_name == entry);
        if (match && fn_bc_size > 0 && fn_bc_off + fn_bc_size <= bc_sec_size) {
            *bc_out = m + bc_sec_off + fn_bc_off;
            *bc_size_out = (size_t)fn_bc_size;
            return true;
        }
        cur = end;
    }

    if (err_out) {
        *err_out = std::string("entry point '") + (entry ? entry : "") +
                   "' not found in MTLB container";
    }
    return false;
}

static void
hex_encode(const uint8_t in[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0xf];
    }
    out[64] = '\0';
}

static uint8_t
encode_base_type(const std::string &base, unsigned bits)
{
    if (base == "float" || base == "half")
        return bits == 16 ? AIR2VH_TYPE_FLOAT16 : AIR2VH_TYPE_FLOAT32;
    if (base == "int" || base == "short" || base == "char")
        return bits <= 16 ? AIR2VH_TYPE_INT16 : AIR2VH_TYPE_INT32;
    if (base == "uint" || base == "ushort" || base == "uchar")
        return bits <= 16 ? AIR2VH_TYPE_UINT16 : AIR2VH_TYPE_UINT32;
    if (base == "bool")
        return AIR2VH_TYPE_BOOL;
    return AIR2VH_TYPE_FLOAT32;
}

static uint8_t
encode_interp(const std::string &interp)
{
    if (interp == "flat")
        return 1; /* flat */
    if (interp == "no_perspective")
        return 2; /* no_perspective */
    return 0;     /* perspective */
}

static uint8_t
encode_sampling(const std::string &sampling)
{
    if (sampling == "centroid")
        return 1; /* centroid */
    if (sampling == "sample")
        return 2; /* sample */
    return 0;     /* center */
}

static uint8_t
encode_binding_kind(const std::string &kind)
{
    if (kind == "ubo") return AIR2VH_BIND_UBO;
    if (kind == "device_buffer") return AIR2VH_BIND_DEVICE_BUFFER;
    if (kind == "threadgroup_buffer") return AIR2VH_BIND_THREADGROUP_BUFFER;
    if (kind == "texture") return AIR2VH_BIND_TEXTURE;
    if (kind == "storage_texture") return AIR2VH_BIND_STORAGE_TEXTURE;
    if (kind == "sampler") return AIR2VH_BIND_SAMPLER;
    if (kind == "constexpr_sampler") return AIR2VH_BIND_CONSTEXPR_SAMPLER;
    if (kind == "fb_fetch") return AIR2VH_BIND_FB_FETCH;
    if (kind == "attribute") return AIR2VH_BIND_ATTRIBUTE;
    return 0;
}

static uint8_t
encode_access(const std::string &access)
{
    if (access == "sample") return 0;
    if (access == "read") return 1;
    if (access == "write" || access == "read_write") return 2;
    return 0;
}

static unsigned
glsl_type_size_slots(const struct glsl_type *type, bool bindless)
{
    (void)bindless;
    return (unsigned)glsl_count_attribute_slots(type, false);
}

static void
lower_nir_io_pan(nir_shader *nir)
{
    NIR_PASS(_, nir, nir_lower_var_copies);
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out),
             UINT32_MAX);
    NIR_PASS(_, nir, nir_lower_io,
             (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out),
             glsl_type_size_slots,
             nir_lower_io_use_interpolated_input_intrinsics);
    NIR_PASS(_, nir, nir_opt_constant_folding);
    pan_nir_lower_mediump_io(nir);
}

static std::string
json_escape(const std::string &s)
{
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r + "\"";
}

static std::string
build_sidecar_json(const air2vh_compiler *c, const AirFrontendResult &fe,
                   const air2vh_program &prog, const char key_hex[65],
                   const uint8_t *code, size_t code_size, uint32_t flags)
{
    llvm::SHA256 h;
    h.update(llvm::ArrayRef<uint8_t>(code, code_size));
    auto bin_dg = h.final();
    char bin_hex[65];
    hex_encode(bin_dg.data(), bin_hex);

    std::ostringstream o;
    o << "{\n";
    o << "  \"format_version\": " << AIR2VH_CACHE_FORMAT_VERSION << ",\n";
    o << "  \"abi_version\": " << AIR2VH_ABI_VERSION << ",\n";
    o << "  \"compiler_id\": " << json_escape(air2vh_compiler_id()) << ",\n";
    o << "  \"gpu_prod_id\": " << c->gpu_prod_id << ",\n";
    o << "  \"key\": \"" << key_hex << "\",\n";
    o << "  \"variant_id\": " << json_escape(fe.function) << ",\n";
    o << "  \"key_inputs\": {\"entry\": " << json_escape(fe.function)
      << ", \"stage\": " << prog.stage << ", \"flags\": " << flags << "},\n";
    o << "  \"reflection\": " << air_reflection_json(fe) << ",\n";
    o << "  \"constant_samplers\": {\"mask\": " << fe.constexpr_sampler_mask << ", \"samplers\": [";
    bool first_cs = true;
    for (const auto &b : fe.bindings) {
        if (b.kind != "constexpr_sampler")
            continue;
        if (!first_cs) o << ", ";
        first_cs = false;
        o << "{\"slot\": " << b.slot << ", \"unique_id\": " << b.unique_id
          << ", \"mali_sampler\": [";
        for (int i = 0; i < 8; i++) o << (i ? "," : "") << b.vh_desc[i];
        o << "], \"mali_sampler_int_border\": [";
        for (int i = 0; i < 4; i++) o << (i ? "," : "") << b.vh_int_border[i];
        o << "]}";
    }
    o << "]},\n";
    o << "  \"blob\": {\"sha256\": \"" << bin_hex << "\", \"code_size\": " << code_size << "},\n";
    o << "  \"vh\": {\"stage\": " << prog.stage
      << ", \"code_size\": " << prog.code_size
      << ", \"spd_count\": " << (unsigned)prog.spd_count
      << ", \"work_reg_count\": [" << (unsigned)prog.work_reg_count[0] << ","
      << (unsigned)prog.work_reg_count[1] << "," << (unsigned)prog.work_reg_count[2] << "]"
      << ", \"fau_slots\": " << prog.fau.slots
      << ", \"fau_reserved_words\": " << prog.fau.reserved_words
      << ", \"tls_size\": " << prog.tls_size
      << ", \"wls_size\": " << prog.wls_size
      << ", \"table_count\": [" << prog.table_count[0] << "," << prog.table_count[1]
      << "," << prog.table_count[2] << "," << prog.table_count[3] << "," << prog.table_count[4] << "]}\n";
    o << "}\n";
    return o.str();
}

static int
compute_cache_key_raw(const air2vh_compiler *c, const air2vh_compile_info *info,
                      uint8_t key[32], std::string *err_out)
{
    if (!info || !info->air || info->air_size == 0 || !key) {
        if (err_out) *err_out = "invalid compile_info or null air buffer";
        return -1;
    }

    const void *bc = nullptr;
    size_t bc_size = 0;
    if (!extract_bitcode_slice(info->air, info->air_size, info->entry,
                               &bc, &bc_size, err_out)) {
        return -1;
    }

    llvm::SHA256 bc_hash;
    bc_hash.update(llvm::ArrayRef<uint8_t>(static_cast<const uint8_t *>(bc), bc_size));
    auto bc_dg = bc_hash.final();

    llvm::SHA256 h;
    auto add_u32 = [&](uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        h.update(llvm::ArrayRef<uint8_t>(b, 4));
    };
    auto add_str = [&](const char *s) {
        uint32_t len = s ? (uint32_t)strlen(s) : 0;
        add_u32(len);
        if (len)
            h.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(s), len));
    };

    const char tag[] = "AIR2VH-1";
    h.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(tag), sizeof(tag)));
    add_u32(AIR2VH_ABI_VERSION);
    add_u32(c ? c->gpu_prod_id : (uint32_t)PAN_PROD_ID(11, 8, 2));
    h.update(llvm::ArrayRef<uint8_t>(bc_dg.data(), bc_dg.size()));
    add_u32(info->stage);
    add_str(info->entry);
    add_u32(info->flags & (AIR2VH_F_NO_IDVS | AIR2VH_F_NONUNIFORM_DISPATCH));

    /* Tier-B layout hash (spec 07 section 7): 32 bytes, or 32 zeros for Tier A. */
    uint8_t layout_dg[32] = {};
    if (info->vs_layout) {
        if (info->vs_layout->blob && info->vs_layout->blob_size > 0) {
            llvm::SHA256 lh;
            lh.update(llvm::ArrayRef<uint8_t>(
                static_cast<const uint8_t *>(info->vs_layout->blob),
                info->vs_layout->blob_size));
            auto d = lh.final();
            memcpy(layout_dg, d.data(), 32);
        } else {
            memcpy(layout_dg, info->vs_layout->layout_hash, 32);
        }
    }
    h.update(llvm::ArrayRef<uint8_t>(layout_dg, 32));

    /* Canonicalize function constants by (index, name). */
    std::vector<air2vh_constant> fcs;
    if (info->constants && info->num_constants > 0) {
        fcs.assign(info->constants, info->constants + info->num_constants);
        std::sort(fcs.begin(), fcs.end(), [](const air2vh_constant &a, const air2vh_constant &b) {
            if (a.index != b.index)
                return a.index < b.index;
            const char *na = a.name ? a.name : "";
            const char *nb = b.name ? b.name : "";
            return strcmp(na, nb) < 0;
        });
    }
    add_u32((uint32_t)fcs.size());
    for (const auto &fc : fcs) {
        add_u32((uint32_t)fc.index);
        add_str(fc.name);
        uint32_t nv = std::min<uint32_t>(fc.num_values, 4u);
        add_u32(nv);
        if (nv) {
            h.update(llvm::ArrayRef<uint8_t>(
                reinterpret_cast<const uint8_t *>(fc.values), nv * sizeof(double)));
        }
    }

    auto final_dg = h.final();
    memcpy(key, final_dg.data(), 32);
    return 0;
}

} /* namespace */

extern "C" {

uint32_t air2vh_api_version(void) { return AIR2VH_API_VERSION; }
uint32_t air2vh_abi_version(void) { return AIR2VH_ABI_VERSION; }
uint32_t air2vh_cache_format_version(void) { return AIR2VH_CACHE_FORMAT_VERSION; }
void air2vh_free(void *p) { free(p); }

const char *
air2vh_compiler_id(void)
{
    return "air2valhall-vh1-r2-mesa-1f9035d-llvm18";
}

air2vh_compiler *
air2vh_compiler_create(uint64_t gpu_id, uint32_t core_count,
                       uint64_t gpu_features, char **error)
{
    if (error)
        *error = nullptr;

    if (gpu_id == 0 || core_count == 0) {
        if (error) *error = strdup("gpu_id and core_count must be non-zero");
        return nullptr;
    }

    unsigned arch = pan_arch(gpu_id);
    if (arch != 11) {
        std::string msg = "unsupported GPU architecture " + std::to_string(arch) +
                          " (libair2valhall targets Valhall arch 11)";
        if (error) *error = strdup(msg.c_str());
        return nullptr;
    }

    /* Resolve pan_model (pan_model.c:109-114). Husky's GPU_ID 0xb8a21020 has
     * PAN_VERSION_STATUS == 0 while pan_model_list for Mali-G715 checks
     * gpu_variant == 4, so try PAN_VERSION_STATUS first and fall back to 4.
     */
    const struct pan_model *model =
        pan_get_model(gpu_id, PAN_VERSION_STATUS(gpu_id), core_count, gpu_features);
    if (!model)
        model = pan_get_model(gpu_id, 4, core_count, gpu_features);
    if (!model)
        model = pan_get_model(0xa8620004u, 4, core_count, gpu_features);
    if (!model) {
        if (error) *error = strdup("pan_get_model failed for gpu_id");
        return nullptr;
    }

    glsl_type_singleton_init_or_ref();

    auto *c = new air2vh_compiler();
    c->model = model;
    c->gpu_id = gpu_id;
    c->gpu_prod_id = pan_prod_id(gpu_id);
    c->core_count = core_count;
    c->gpu_features = gpu_features;
    c->shader_present = (core_count == 7) ? 0x01110055ull : ((1ull << core_count) - 1ull);
    return c;
}

void
air2vh_compiler_destroy(air2vh_compiler *c)
{
    if (!c)
        return;
    glsl_type_singleton_decref();
    delete c;
}

void
air2vh_compiler_set_shader_present(air2vh_compiler *c, uint64_t shader_present)
{
    if (!c)
        return;
    std::lock_guard<std::mutex> lock(c->mtx);
    if (shader_present != 0)
        c->shader_present = shader_present;
}

int
air2vh_cache_key(air2vh_compiler *c, const air2vh_compile_info *info,
                 char key_hex[65], char **error)
{
    if (error)
        *error = nullptr;
    if (!key_hex) {
        if (error) *error = strdup("key_hex is NULL");
        return -1;
    }
    uint8_t key[32] = {};
    std::string err;
    if (compute_cache_key_raw(c, info, key, &err) != 0) {
        if (error) *error = strdup(err.c_str());
        return -1;
    }
    hex_encode(key, key_hex);
    return 0;
}

int
air2vh_compile(air2vh_compiler *c, const air2vh_compile_info *info,
               air2vh_result *res)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!res)
        return -1;
    memset(res, 0, sizeof(*res));

    auto fail = [&](const std::string &msg) -> int {
        res->error = strdup(msg.c_str());
        res->compile_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        return -1;
    };

    if (!c)
        return fail("air2valhall: ERROR: compiler is NULL");
    if (!info || !info->air || info->air_size == 0)
        return fail("air2valhall: ERROR: air bitcode is NULL or empty");
    if (info->stage > AIR2VH_STAGE_KERNEL)
        return fail("air2valhall: ERROR: invalid stage");

    std::lock_guard<std::mutex> lock(c->mtx);

    const void *bc = nullptr;
    size_t bc_size = 0;
    std::string slice_err;
    if (!extract_bitcode_slice(info->air, info->air_size, info->entry,
                               &bc, &bc_size, &slice_err)) {
        return fail("air2valhall: ERROR: " + slice_err);
    }

    uint8_t raw_key[32] = {};
    if (compute_cache_key_raw(c, info, raw_key, &slice_err) != 0)
        return fail("air2valhall: ERROR: " + slice_err);
    hex_encode(raw_key, res->key_hex);

    AirOptions opts;
    if (info->entry && *info->entry)
        opts.entry = info->entry;
    if (info->stage == AIR2VH_STAGE_VERTEX)
        opts.expectStage = MESA_SHADER_VERTEX;
    else if (info->stage == AIR2VH_STAGE_FRAGMENT)
        opts.expectStage = MESA_SHADER_FRAGMENT;
    else if (info->stage == AIR2VH_STAGE_KERNEL)
        opts.expectStage = MESA_SHADER_COMPUTE;
    else
        opts.expectStage = -1;

    opts.nonuniformDispatch = (info->flags & AIR2VH_F_NONUNIFORM_DISPATCH) != 0;

    if (info->constants && info->num_constants > 0) {
        for (uint32_t i = 0; i < info->num_constants; i++) {
            const air2vh_constant &f = info->constants[i];
            uint32_t nv = std::min<uint32_t>(f.num_values, 4u);
            if (nv == 0) nv = 1;
            std::vector<double> vals(f.values, f.values + nv);
            if (f.index >= 0)
                opts.fc[f.index] = vals;
            else if (f.name && *f.name)
                opts.fcByName[f.name] = vals;
        }
    }

    const nir_shader_compiler_options *nopts =
        pan_get_nir_shader_compiler_options(11, MESA_SHADER_FRAGMENT, false);

    AirFrontendResult fe;
    struct util_dynarray binary;
    util_dynarray_init(&binary, nullptr);

    try {
        fe = air_frontend_mem(bc, bc_size, opts, nopts);
        nir_shader *nir = fe.nir;

        /* Update NIR compiler options to match the actual stage. */
        nir->options = pan_get_nir_shader_compiler_options(11, fe.stage, false);

        /* Per-sample shading detection (spec 07 6.10, air2ir3/library.cpp:319-338). */
        bool per_sample_if_msaa = false;
        if (fe.stage == MESA_SHADER_FRAGMENT) {
            for (const auto &sv : fe.sysvals) {
                if (sv == "sample_id" || sv == "sample_mask_in")
                    per_sample_if_msaa = true;
            }
            for (const auto &vy : fe.varyings) {
                if (vy.sampling == "sample")
                    per_sample_if_msaa = true;
            }
            if (per_sample_if_msaa)
                nir->info.fs.uses_sample_shading = true;
        }

        /* Step 1: Pre-NIR preparation (Tier-B FS varying remap, nonuniform dispatch guard). */
        air2vh_prepare_pre_nir(nir, fe, info->flags, info->vs_layout);
        nir_lower_compute_system_values(nir, NULL);

        /* Step 2: Preprocess NIR for Valhall v11. */
        pan_preprocess_nir(nir, 11);

        /* Step 3: Lower bindings to VH-ABI 1 (SRT handles, FAU sysval markers, inline const pool). */
        uint16_t table_count[AIR2VH_T_COUNT] = {};
        air2vh_lower_bindings(nir, fe, table_count);

        /* Step 4: Lower shader IO variables and build varying layout. */
        struct pan_varying_layout vs_varying_layout = {};
        struct pan_varying_layout fs_tier_b_layout = {};
        struct pan_compile_inputs inputs = {};
        inputs.arch = 11;
        inputs.model = c->model;
        inputs.no_idvs = (info->flags & AIR2VH_F_NO_IDVS) != 0;
        inputs.fau.promote_immediates = true;

        bool is_tier_b = false;
        if (fe.stage == MESA_SHADER_VERTEX) {
            nir_assign_io_var_locations(nir, nir_var_shader_out);
            lower_nir_io_pan(nir);
            pan_varying_collect_formats(&vs_varying_layout, nir, 11);
            pan_build_varying_layout_compact(&vs_varying_layout, nir, 11);
            inputs.varying_layout = &vs_varying_layout;
        } else if (fe.stage == MESA_SHADER_FRAGMENT) {
            nir_assign_io_var_locations(nir, nir_var_shader_in);
            nir_assign_io_var_locations(nir, nir_var_shader_out);
            if (info->vs_layout && info->vs_layout->blob &&
                info->vs_layout->blob_size >= sizeof(struct pan_varying_layout)) {
                is_tier_b = true;
                memcpy(&fs_tier_b_layout, info->vs_layout->blob,
                       sizeof(struct pan_varying_layout));
                /* Ensure any unmatched FS input variable has a valid slot entry so
                 * pan_nir_lower_fs_inputs.c:110 does not assert(slot).
                 */
                nir_foreach_shader_in_variable(var, nir) {
                    gl_varying_slot loc = (gl_varying_slot)var->data.location;
                    if (!pan_varying_layout_find_slot(&fs_tier_b_layout, loc) &&
                        fs_tier_b_layout.count < PAN_MAX_VARYINGS) {
                        struct pan_varying_slot &s =
                            fs_tier_b_layout.slots[fs_tier_b_layout.count++];
                        s.location = loc;
                        s.alu_type = nir_get_nir_type_for_glsl_type(var->type);
                        s.ncomps = glsl_get_vector_elements(var->type);
                        s.section = PAN_VARYING_SECTION_GENERIC;
                        s.offset = 0;
                    }
                }
                inputs.varying_layout = &fs_tier_b_layout;
            }
            lower_nir_io_pan(nir);
        }

        /* pan_shader_update_info reads nir->info (quad helpers for implicit-LOD tex and
         * derivatives, discard, barriers); panvk gathers at the same point
         * [M:vulkan/panvk_vX_shader.c:972]. Without it FS never request helper threads. */
        nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

        /* Step 5: Postprocess NIR for Valhall v11. */
        struct pan_shader_info sinfo = {};
        pan_postprocess_nir(nir, &inputs, &sinfo);

        /* Step 6: Lower sysvals to FAU push constants and pack FAU slots. */
        vh_fau_plan fau_plan = {};
        air2vh_lower_sysvals_and_fau(nir, c->shader_present, inputs.no_idvs, &fau_plan);
        inputs.fau.reserved = 2u * vh_fau_sysval_slots(&fau_plan);

        /* Step 7: Compile NIR to Valhall machine code. */
        pan_shader_compile(nir, &inputs, &binary, &sinfo);

        /* Step 8: Allocate & populate air2vh_program + air2vh_program_ext (spec 07 8.2). */
        auto *prog_ext = static_cast<VhProgramWithExt *>(calloc(1, sizeof(VhProgramWithExt)));
        if (!prog_ext)
            throw std::bad_alloc();

        air2vh_program &p = prog_ext->prog;
        p.struct_size = sizeof(air2vh_program);
        p.abi_version = AIR2VH_ABI_VERSION;
        p.stage = (fe.stage == MESA_SHADER_VERTEX)   ? AIR2VH_STAGE_VERTEX
                : (fe.stage == MESA_SHADER_FRAGMENT) ? AIR2VH_STAGE_FRAGMENT
                                                     : AIR2VH_STAGE_KERNEL;
        p.code_size = (uint32_t)binary.size;

        if (sinfo.stats.valhall.constant_data_size > 0) {
            p.constant_pool_size = sinfo.stats.valhall.constant_data_size;
            p.constant_pool_offset =
                ALIGN_POT(sinfo.stats.valhall.instrs * 8u + 8u, 128u);
        } else if (sinfo.stats_idvs_varying.valhall.constant_data_size > 0) {
            p.constant_pool_size = sinfo.stats_idvs_varying.valhall.constant_data_size;
            p.constant_pool_offset =
                sinfo.vs.secondary_offset +
                ALIGN_POT(sinfo.stats_idvs_varying.valhall.instrs * 8u + 8u, 128u);
        }

        if (fe.stage == MESA_SHADER_VERTEX) {
            p.entry_offset[0] = 0;
            p.entry_offset[1] = sinfo.vs.idvs ? sinfo.vs.no_psiz_offset : 0;
            p.entry_offset[2] = sinfo.vs.idvs ? sinfo.vs.secondary_offset : 0;
            p.spd_count = sinfo.vs.secondary_enable ? 3 : 2;
            p.work_reg_count[0] = (uint8_t)sinfo.work_reg_count;
            p.work_reg_count[1] = (uint8_t)sinfo.work_reg_count;
            p.work_reg_count[2] = (uint8_t)(sinfo.vs.secondary_work_reg_count
                                                ? sinfo.vs.secondary_work_reg_count
                                                : sinfo.work_reg_count);
            p.preload[0] = sinfo.preload;
            p.preload[1] = sinfo.preload;
            p.preload[2] = sinfo.vs.secondary_preload;
        } else {
            p.entry_offset[0] = 0;
            p.spd_count = 1;
            p.work_reg_count[0] = (uint8_t)sinfo.work_reg_count;
            p.preload[0] = sinfo.preload;
        }

        p.ftz_fp16 = sinfo.ftz_fp16 ? 1 : 0;
        p.ftz_fp32 = sinfo.ftz_fp32 ? 1 : 0;
        p.requires_helper_threads =
            (fe.stage == MESA_SHADER_FRAGMENT && sinfo.contains_barrier) ? 1 : 0;
        p.tls_size = sinfo.tls_size;
        p.wls_size = (fe.stage == MESA_SHADER_COMPUTE)
                         ? std::max<uint32_t>(sinfo.wls_size, fe.shared_bytes)
                         : sinfo.wls_size;

        /* FAU sources and promoted immediates. */
        uint32_t imm_words[AIR2VH_FAU_WORDS] = {};
        uint32_t imm_values[AIR2VH_FAU_WORDS] = {};
        uint32_t n_imm = 0;
        pan_fau_foreach_imm(&sinfo.fau, w) {
            if (n_imm < AIR2VH_FAU_WORDS) {
                imm_words[n_imm] = w;
                imm_values[n_imm] = sinfo.fau.words[w].constant;
                n_imm++;
            }
        }
        if (vh_fau_emit(&fau_plan, &p, sinfo.fau.count,
                        imm_words, imm_values, n_imm) != 0) {
            free(prog_ext);
            throw AirError("FAU slot budget (64 slots) exceeded after constant promotion");
        }

        memcpy(p.table_count, table_count, sizeof(table_count));

        if (fe.stage == MESA_SHADER_VERTEX) {
            p.vs.idvs = sinfo.vs.idvs ? 1 : 0;
            p.vs.secondary_enable = sinfo.vs.secondary_enable ? 1 : 0;
            p.vs.writes_point_size = fe.writes_point_size ? 1 : 0;
            p.vs.needs_extended_fifo = sinfo.vs.needs_extended_fifo ? 1 : 0;
            uint32_t attr_mask = 0;
            for (const auto &at : fe.attributes) {
                if (at.index >= 0 && at.index < 32)
                    attr_mask |= (1u << at.index);
            }
            p.vs.attribute_mask = attr_mask;
            p.vs.noperspective = sinfo.varyings.noperspective;

            auto *lblob = static_cast<vh_vs_layout_blob *>(calloc(1, sizeof(vh_vs_layout_blob)));
            if (lblob) {
                memcpy(lblob->pan_bytes, &sinfo.varyings.formats,
                       sizeof(struct pan_varying_layout));
                lblob->magic = VH_VS_LAYOUT_MAGIC;
                for (const auto &vy : fe.varyings) {
                    if (vy.slot >= 0 && vy.slot < 32) {
                        uint32_t h24 = air2vh_name_hash24(vy.name.c_str());
                        lblob->name_hash24[vy.slot][0] = (uint8_t)(h24 & 0xffu);
                        lblob->name_hash24[vy.slot][1] = (uint8_t)((h24 >> 8) & 0xffu);
                        lblob->name_hash24[vy.slot][2] = (uint8_t)((h24 >> 16) & 0xffu);
                    }
                }
                llvm::SHA256 lh;
                lh.update(llvm::ArrayRef<uint8_t>(
                    reinterpret_cast<const uint8_t *>(lblob), sizeof(*lblob)));
                auto ldg = lh.final();

                res->vs_layout_out.struct_size = sizeof(air2vh_vs_layout);
                memcpy(res->vs_layout_out.layout_hash, ldg.data(), 32);
                res->vs_layout_out.blob_size = sizeof(vh_vs_layout_blob);
                res->vs_layout_out.blob = lblob;
                p.vs.layout = res->vs_layout_out;
            }
        } else if (fe.stage == MESA_SHADER_FRAGMENT) {
            p.fs.reads_frag_coord = sinfo.fs.reads_frag_coord ? 1 : 0;
            p.fs.reads_point_coord = sinfo.fs.reads_point_coord ? 1 : 0;
            p.fs.reads_primitive_id = sinfo.fs.reads_primitive_id ? 1 : 0;
            p.fs.reads_face = sinfo.fs.reads_face ? 1 : 0;
            p.fs.can_discard = (sinfo.fs.can_discard || fe.uses_discard) ? 1 : 0;
            p.fs.writes_depth = (sinfo.fs.writes_depth || fe.writes_depth) ? 1 : 0;
            p.fs.writes_stencil = sinfo.fs.writes_stencil ? 1 : 0;
            p.fs.writes_coverage = sinfo.fs.writes_coverage ? 1 : 0;
            p.fs.sidefx = sinfo.writes_global ? 1 : 0;
            p.fs.sample_shading = sinfo.fs.sample_shading ? 1 : 0;
            p.fs.early_fragment_tests = sinfo.fs.early_fragment_tests ? 1 : 0;
            p.fs.can_early_z = sinfo.fs.can_early_z ? 1 : 0;
            p.fs.can_fpk = sinfo.fs.can_fpk ? 1 : 0;
            p.fs.untyped_color_outputs = sinfo.fs.untyped_color_outputs ? 1 : 0;
            p.fs.per_sample_if_msaa = per_sample_if_msaa ? 1 : 0;
            p.fs.uses_fb_fetch = fe.uses_fb_fetch ? 1 : 0;
            p.fs.outputs_read = sinfo.fs.outputs_read;
            uint32_t fbf_mask = 0;
            for (const auto &rt : fe.render_targets) {
                if (rt.fb_fetch && rt.index >= 0 && rt.index < 8)
                    fbf_mask |= (1u << rt.index);
            }
            p.fs.fb_fetch_rt_mask = fbf_mask;

            /* Pack early-ZS LUT into 24 bytes (1 byte per state, mali_state.c:302-312). */
            struct pan_earlyzs_lut lut = pan_earlyzs_analyze(&sinfo, 11);
            unsigned lut_idx = 0;
            for (unsigned w = 0; w < 2; w++) {
                for (unsigned a = 0; a < 2; a++) {
                    for (unsigned z = 0; z < 2; z++) {
                        for (unsigned r = 0; r < 3; r++) {
                            struct pan_earlyzs_state st = lut.states[w][a][z][r];
                            p.fs.earlyzs_lut[lut_idx++] =
                                (uint8_t)((st.update & 3u) |
                                          ((st.kill & 3u) << 2) |
                                          ((st.shader_readonly_zs ? 1u : 0u) << 4));
                        }
                    }
                }
            }

            p.fs.uses_ld_var = sinfo.bifrost.uses_ld_var ? 1 : 0;
            if (p.fs.uses_ld_var) {
                p.fs.varying_desc_count = (uint16_t)sinfo.varyings.formats.count;
                p.table_count[AIR2VH_T_DRIVER] = (uint16_t)sinfo.varyings.formats.count;
            }

            for (unsigned i = 0; i < 8; i++)
                p.fs.rt[i].index = (uint8_t)i;
            for (const auto &rt : fe.render_targets) {
                if (rt.index >= 0 && rt.index < 8) {
                    p.fs.rt[rt.index].index = (uint8_t)rt.index;
                    p.fs.rt[rt.index].comps = (uint8_t)rt.comps;
                    p.fs.rt[rt.index].base_type = encode_base_type(rt.base, rt.bits);
                    p.fs.rt[rt.index].written = rt.written ? 1 : 0;
                }
            }
        } else if (fe.stage == MESA_SHADER_COMPUTE) {
            p.cs.local_size_hint[0] = nir->info.workgroup_size[0];
            p.cs.local_size_hint[1] = nir->info.workgroup_size[1];
            p.cs.local_size_hint[2] = nir->info.workgroup_size[2];
            p.cs.allow_merging_workgroups = 0;
        }

        p.contains_barrier = sinfo.contains_barrier ? 1 : 0;
        p.writes_global = sinfo.writes_global ? 1 : 0;

        /* Step 9: Populate air2vh_program_ext (air2vh_ext.h). */
        air2vh_program_ext &ext = prog_ext->ext;
        ext.magic = AIR2VH_EXT_MAGIC;
        ext.version = AIR2VH_EXT_VERSION;
        ext.size = sizeof(air2vh_program_ext);
        ext.tier_b = is_tier_b ? 1 : 0;
        ext.uses_dummy_sampler =
            (p.table_count[AIR2VH_T_SAMPLERS] > AIR2VH_DUMMY_SAMPLER_INDEX) ? 1 : 0;

        if (fe.stage == MESA_SHADER_VERTEX || fe.stage == MESA_SHADER_FRAGMENT) {
            const struct pan_varying_layout &vlay = sinfo.varyings.formats;
            ext.generic_size_B = (fe.stage == MESA_SHADER_VERTEX)
                                     ? (uint16_t)vlay.generic_size_B
                                     : 0;
            uint32_t nv = std::min<uint32_t>(vlay.count, AIR2VH_EXT_MAX_VARYINGS);
            ext.n_varyings = nv;
            for (uint32_t i = 0; i < nv; i++) {
                const struct pan_varying_slot &src_s = vlay.slots[i];
                air2vh_varying_slot &dst_s = ext.varyings[i];
                dst_s.location = (uint8_t)src_s.location;
                dst_s.section = (uint8_t)src_s.section;
                dst_s.alu_type = (uint8_t)src_s.alu_type;
                dst_s.ncomps = (uint8_t)src_s.ncomps;
                dst_s.offset = (fe.stage == MESA_SHADER_VERTEX)
                                   ? (int16_t)src_s.offset
                                   : -1;
                if (src_s.location >= VARYING_SLOT_VAR0 &&
                    src_s.location <= VARYING_SLOT_VAR31) {
                    int slot_idx = (int)(src_s.location - VARYING_SLOT_VAR0);
                    for (const auto &vy : fe.varyings) {
                        if (vy.slot == slot_idx) {
                            snprintf(dst_s.name, sizeof(dst_s.name), "%s", vy.name.c_str());
                            dst_s.interp = encode_interp(vy.interp);
                            dst_s.sampling = encode_sampling(vy.sampling);
                            break;
                        }
                    }
                }
            }
        }

        for (const auto &at : fe.attributes) {
            if (ext.n_attributes >= AIR2VH_EXT_MAX_ATTRIBUTES)
                break;
            air2vh_attribute &dst = ext.attributes[ext.n_attributes++];
            dst.index = (uint8_t)at.index;
            dst.comps = (uint8_t)at.comps;
            dst.bits = (uint8_t)at.bits;
            dst.base_type = encode_base_type(at.base, at.bits);
        }

        for (const auto &bd : fe.bindings) {
            if (bd.kind == "constexpr_sampler" &&
                ext.n_constexpr_samplers < AIR2VH_EXT_MAX_CSAMPLERS) {
                ext.constexpr_sampler_mask |= (1u << bd.slot);
                air2vh_constexpr_sampler &cs =
                    ext.constexpr_samplers[ext.n_constexpr_samplers++];
                cs.slot = (uint32_t)bd.slot;
                cs.unique_id = bd.unique_id;
                cs.air_word = bd.sampler_word;
                memcpy(cs.mali_sampler, bd.vh_desc, sizeof(cs.mali_sampler));
                memcpy(cs.int_border, bd.vh_int_border, sizeof(cs.int_border));
            }
            if (ext.n_bindings >= AIR2VH_EXT_MAX_BINDINGS)
                continue;
            air2vh_binding &dst = ext.bindings[ext.n_bindings++];
            dst.kind = encode_binding_kind(bd.kind);
            dst.used = bd.used ? 1 : 0;
            dst.present = bd.present ? 1 : 0;
            dst.table = 0xffu;
            dst.metal_index = (int16_t)bd.metal_index;
            dst.sysval = 0;
            dst.access = encode_access(bd.access);
            dst.handle = 0;
            dst.minimum_bytes = bd.minimum_bytes;
            dst.fau_byte_offset = 0;

            if (bd.kind == "ubo" && bd.metal_index >= 0) {
                dst.table = AIR2VH_T_BUFFERS;
                dst.handle = AIR2VH_RES_HANDLE(AIR2VH_T_BUFFERS, bd.metal_index);
            } else if (bd.kind == "texture" && bd.metal_index >= 0) {
                dst.table = AIR2VH_T_TEXTURES;
                dst.handle = AIR2VH_RES_HANDLE(AIR2VH_T_TEXTURES, bd.metal_index);
            } else if (bd.kind == "storage_texture" && bd.metal_index >= 0) {
                dst.table = AIR2VH_T_STORAGE;
                dst.handle = AIR2VH_RES_HANDLE(AIR2VH_T_STORAGE, bd.metal_index);
            } else if ((bd.kind == "sampler" || bd.kind == "constexpr_sampler") &&
                       bd.slot >= 0) {
                dst.table = AIR2VH_T_SAMPLERS;
                dst.metal_index = (int16_t)(bd.kind == "constexpr_sampler" ? bd.slot : bd.metal_index);
                dst.handle = AIR2VH_RES_HANDLE(AIR2VH_T_SAMPLERS, bd.slot);
            } else if (bd.kind == "attribute" && bd.slot >= 0) {
                dst.table = AIR2VH_T_DRIVER;
                dst.handle = AIR2VH_RES_HANDLE(AIR2VH_T_DRIVER, bd.slot);
            } else if (bd.kind == "device_buffer" && bd.metal_index >= 0) {
                dst.sysval = (fe.stage == MESA_SHADER_COMPUTE)
                                 ? AIR2VH_SV_CS_BUFFER_VA
                                 : AIR2VH_SV_GFX_BUFFER_VA;
                dst.fau_byte_offset = (fe.stage == MESA_SHADER_COMPUTE)
                                          ? (uint32_t)offsetof(air2vh_sysvals_cs, buffer_va[0]) + (uint32_t)bd.metal_index * 8u
                                          : (uint32_t)offsetof(air2vh_sysvals_gfx, buffer_va[0]) + (uint32_t)bd.metal_index * 8u;
            } else if (bd.kind == "threadgroup_buffer" && bd.metal_index >= 0) {
                dst.sysval = AIR2VH_SV_TG_OFFSET;
                dst.fau_byte_offset = (uint32_t)offsetof(air2vh_sysvals_cs, tg_offset[0]) + (uint32_t)bd.metal_index * 4u;
            } else if (bd.kind == "fb_fetch" && bd.metal_index >= 0) {
                dst.sysval = AIR2VH_SV_RT_CONV;
                dst.fau_byte_offset = (uint32_t)offsetof(air2vh_sysvals_gfx, rt_conv[0]) + (uint32_t)bd.metal_index * 4u;
            }
        }

        for (int rt_idx : fe.requires_no_attachment) {
            if (rt_idx >= 0 && rt_idx < 32)
                ext.requires_no_attachment |= (1u << rt_idx);
        }
        ext.threadgroup_bytes = fe.shared_bytes;
        ext.constant_data_bytes = fe.constant_data_bytes;

        /* Step 10: Allocate code & JSON sidecar. */
        void *code_copy = malloc(binary.size ? binary.size : 8);
        if (!code_copy) {
            free(prog_ext);
            throw std::bad_alloc();
        }
        if (binary.size > 0)
            memcpy(code_copy, binary.data, binary.size);

        std::string json_str = build_sidecar_json(
            c, fe, p, res->key_hex,
            static_cast<const uint8_t *>(code_copy), binary.size, info->flags);
        char *json_copy = static_cast<char *>(malloc(json_str.size() + 1));
        if (!json_copy) {
            free(code_copy);
            free(prog_ext);
            throw std::bad_alloc();
        }
        memcpy(json_copy, json_str.data(), json_str.size() + 1);

        res->code = code_copy;
        res->code_size = binary.size;
        res->sidecar_json = json_copy;
        res->sidecar_size = json_str.size();
        res->program = &prog_ext->prog;
        res->error = nullptr;
        res->compile_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

        util_dynarray_fini(&binary);
        ralloc_free(nir);
        return 0;
    } catch (const AirError &e) {
        util_dynarray_fini(&binary);
        if (fe.nir)
            ralloc_free(fe.nir);
        if (res->vs_layout_out.blob) {
            free(const_cast<void *>(res->vs_layout_out.blob));
            memset(&res->vs_layout_out, 0, sizeof(res->vs_layout_out));
        }
        return fail(std::string("air2valhall: UNSUPPORTED/ERROR: ") + e.what());
    } catch (const std::exception &e) {
        util_dynarray_fini(&binary);
        if (fe.nir)
            ralloc_free(fe.nir);
        if (res->vs_layout_out.blob) {
            free(const_cast<void *>(res->vs_layout_out.blob));
            memset(&res->vs_layout_out, 0, sizeof(res->vs_layout_out));
        }
        return fail(std::string("air2valhall: ERROR: ") + e.what());
    }
}

void
air2vh_result_free(air2vh_result *r)
{
    if (!r)
        return;
    free(const_cast<void *>(r->code));
    free(r->sidecar_json);
    free(r->program);
    free(const_cast<void *>(r->vs_layout_out.blob));
    free(r->error);
    memset(r, 0, sizeof(*r));
}

static enum pipe_blend_func
mtl_to_pipe_blend_op(uint8_t op)
{
    switch (op) {
    case 0: return PIPE_BLEND_ADD;
    case 1: return PIPE_BLEND_SUBTRACT;
    case 2: return PIPE_BLEND_REVERSE_SUBTRACT;
    case 3: return PIPE_BLEND_MIN;
    case 4: return PIPE_BLEND_MAX;
    default: return PIPE_BLEND_ADD;
    }
}

static enum pipe_blendfactor
mtl_to_pipe_blend_factor(uint8_t f)
{
    switch (f) {
    case 0:  return PIPE_BLENDFACTOR_ZERO;
    case 1:  return PIPE_BLENDFACTOR_ONE;
    case 2:  return PIPE_BLENDFACTOR_SRC_COLOR;
    case 3:  return PIPE_BLENDFACTOR_INV_SRC_COLOR;
    case 4:  return PIPE_BLENDFACTOR_SRC_ALPHA;
    case 5:  return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
    case 6:  return PIPE_BLENDFACTOR_DST_COLOR;
    case 7:  return PIPE_BLENDFACTOR_INV_DST_COLOR;
    case 8:  return PIPE_BLENDFACTOR_DST_ALPHA;
    case 9:  return PIPE_BLENDFACTOR_INV_DST_ALPHA;
    case 10: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 11: return PIPE_BLENDFACTOR_CONST_COLOR;
    case 12: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
    case 13: return PIPE_BLENDFACTOR_CONST_ALPHA;
    case 14: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
    case 15: return PIPE_BLENDFACTOR_SRC1_COLOR;
    case 16: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
    case 17: return PIPE_BLENDFACTOR_SRC1_ALPHA;
    case 18: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
    default: return PIPE_BLENDFACTOR_ONE;
    }
}

static nir_alu_type
decode_nir_alu_type(uint8_t base_type)
{
    switch (base_type) {
    case AIR2VH_TYPE_FLOAT16: return nir_type_float16;
    case AIR2VH_TYPE_FLOAT32: return nir_type_float32;
    case AIR2VH_TYPE_INT16:   return nir_type_int16;
    case AIR2VH_TYPE_INT32:   return nir_type_int32;
    case AIR2VH_TYPE_UINT16:  return nir_type_uint16;
    case AIR2VH_TYPE_UINT32:  return nir_type_uint32;
    default:                  return nir_type_float32;
    }
}

static bool
lower_blend_const_load(nir_builder *b, nir_instr *instr, void *data)
{
    (void)data;
    if (instr->type != nir_instr_type_intrinsic)
        return false;
    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
    if (intr->intrinsic != nir_intrinsic_load_blend_const_color_rgba)
        return false;
    b->cursor = nir_before_instr(instr);
    /* Blend constants live at FAU words 0..3 (spec 07 6.10, panvk_vX_blend.c:42-46). */
    nir_def *c = nir_load_push_constant(b, intr->def.num_components,
                                        intr->def.bit_size, nir_imm_int(b, 0));
    nir_def_replace(&intr->def, c);
    return true;
}

int
air2vh_compile_blend_shader(air2vh_compiler *c, const void *key,
                            size_t key_size, air2vh_result *res)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!res)
        return -1;
    memset(res, 0, sizeof(*res));

    auto fail = [&](const char *msg) -> int {
        res->error = strdup(msg);
        return -1;
    };

    if (!c || !key)
        return fail("air2vh_compile_blend_shader: NULL compiler or key");

    std::lock_guard<std::mutex> lock(c->mtx);

    struct pan_blend_state state = {};
    nir_alu_type src0_type = nir_type_float32;
    nir_alu_type src1_type = nir_type_float32;
    unsigned rt = 0;

    if (key_size == sizeof(air2vh_blend_key) &&
        static_cast<const air2vh_blend_key *>(key)->struct_size == sizeof(air2vh_blend_key)) {
        const auto *bk = static_cast<const air2vh_blend_key *>(key);
        if (bk->rt >= 8)
            return fail("air2vh_compile_blend_shader: rt >= 8");
        rt = bk->rt;
        state.alpha_to_one = bk->alpha_to_one != 0;
        state.rt_count = rt + 1;
        state.rts[rt].format = (enum pipe_format)bk->pipe_format;
        if (state.rts[rt].format == PIPE_FORMAT_NONE)
            state.rts[rt].format = PIPE_FORMAT_B8G8R8A8_UNORM;
        state.rts[rt].nr_samples = bk->nr_samples ? bk->nr_samples : 1;
        state.rts[rt].equation.blend_enable = bk->blending_enabled ? 1 : 0;
        state.rts[rt].equation.rgb_func = mtl_to_pipe_blend_op(bk->rgb_op);
        state.rts[rt].equation.rgb_src_factor = mtl_to_pipe_blend_factor(bk->src_rgb);
        state.rts[rt].equation.rgb_dst_factor = mtl_to_pipe_blend_factor(bk->dst_rgb);
        state.rts[rt].equation.alpha_func = mtl_to_pipe_blend_op(bk->alpha_op);
        state.rts[rt].equation.alpha_src_factor = mtl_to_pipe_blend_factor(bk->src_alpha);
        state.rts[rt].equation.alpha_dst_factor = mtl_to_pipe_blend_factor(bk->dst_alpha);
        state.rts[rt].equation.color_mask = bk->write_mask & 0xfu;
        src0_type = decode_nir_alu_type(bk->src0_type);
        src1_type = bk->src1_type ? decode_nir_alu_type(bk->src1_type) : src0_type;
    } else if (key_size == sizeof(struct pan_blend_shader_key)) {
        const auto *pk = static_cast<const struct pan_blend_shader_key *>(key);
        rt = pk->rt;
        state.alpha_to_one = pk->alpha_to_one != 0;
        state.logicop_enable = pk->logicop_enable != 0;
        state.logicop_func = (enum pipe_logicop)pk->logicop_func;
        state.rt_count = rt + 1;
        state.rts[rt].format = pk->format != PIPE_FORMAT_NONE
                                   ? pk->format
                                   : PIPE_FORMAT_B8G8R8A8_UNORM;
        state.rts[rt].nr_samples = pk->nr_samples ? pk->nr_samples : 1;
        state.rts[rt].equation = pk->equation;
        src0_type = pk->src0_type ? pk->src0_type : nir_type_float32;
        src1_type = pk->src1_type ? pk->src1_type : src0_type;
    } else {
        return fail("air2vh_compile_blend_shader: unrecognized key size");
    }

    if (state.rts[rt].equation.color_mask == 0)
        state.rts[rt].equation.color_mask = 0xfu;

    nir_shader *nir = pan_blend_create_shader_v11(&state, src0_type, src1_type, rt);
    NIR_PASS(_, nir, nir_shader_instructions_pass, lower_blend_const_load,
             nir_metadata_control_flow, nullptr);

    struct pan_compile_inputs inputs = {};
    inputs.arch = 11;
    inputs.model = c->model;
    inputs.is_blend = true;
    inputs.fau.reserved = 4;

    pan_preprocess_nir(nir, 11);
    struct pan_shader_info sinfo = {};
    pan_postprocess_nir(nir, &inputs, &sinfo);

    struct util_dynarray binary;
    util_dynarray_init(&binary, nullptr);
    pan_shader_compile(nir, &inputs, &binary, &sinfo);

    void *code_copy = malloc(binary.size ? binary.size : 8);
    auto *prog_ext = static_cast<VhProgramWithExt *>(calloc(1, sizeof(VhProgramWithExt)));
    if (!code_copy || !prog_ext) {
        free(code_copy);
        free(prog_ext);
        util_dynarray_fini(&binary);
        ralloc_free(nir);
        return fail("air2vh_compile_blend_shader: out of memory");
    }
    if (binary.size > 0)
        memcpy(code_copy, binary.data, binary.size);

    air2vh_program &p = prog_ext->prog;
    p.struct_size = sizeof(air2vh_program);
    p.abi_version = AIR2VH_ABI_VERSION;
    p.stage = AIR2VH_STAGE_FRAGMENT;
    p.code_size = (uint32_t)binary.size;
    p.spd_count = 1;
    p.work_reg_count[0] = (uint8_t)sinfo.work_reg_count;
    p.preload[0] = sinfo.preload;

    prog_ext->ext.magic = AIR2VH_EXT_MAGIC;
    prog_ext->ext.version = AIR2VH_EXT_VERSION;
    prog_ext->ext.size = sizeof(air2vh_program_ext);

    res->code = code_copy;
    res->code_size = binary.size;
    res->program = &p;
    res->compile_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

    util_dynarray_fini(&binary);
    ralloc_free(nir);
    return 0;
}

uint32_t
air2vh_varying_pipe_format(uint8_t alu_type, uint8_t ncomps)
{
    if (ncomps < 1 || ncomps > 4 || alu_type == 0)
        return 0;
    return (uint32_t)pan_varying_format((nir_alu_type)alu_type, ncomps);
}

} /* extern "C" */
