/*
 * mali_compute_build.h - build a raw Mali-G715 (arch 11) compute job: shader, descriptors, CS stream.
 * -------------------------------------------------------------------------
 * Intended repo path: tests/device/mali_compute_build.h   (C++17, header-only)
 * Shared by tests/device/mali_compute_job.cpp (the device CLI, Boot 3) and
 * tests/host/test_compute_job.cpp (replays the build on the host and checks every byte/word).
 *
 * Nothing here talks to a device: it fills a CPU buffer that will become one GPU resource and tells the
 * caller the GPU addresses inside it.  The recipe is spec 06 section 5.3 (the "HWR" minimal compute
 * runner template), in our own code:
 *
 *   job buffer layout (one resource, GPU-executable, first 4 GiB):
 *     [FAU 64-bit slots][pad][shader code @128][TSD @64][SPD @32][CS stream @64]
 *   FAU slot 0 = &in, slot 1 = &out, slot 2 = {mul, add}   (device/kernels/mul_add.vhasm ABI)
 *
 *   CS stream (user chunk; the kernel's 11-instruction ring wrapper CALLs it, spec 06 3.2):
 *     REQ_RESOURCE compute; SRT_0 = 0 (no resource table: buffers travel by pointer in the FAU);
 *     FAU_0 = fau | slots << 56; SPD_0; TSD_0; GLOBAL_ATTRIBUTE_OFFSET = 0; WG_SIZE; JOB_OFFSET_* = 0;
 *     JOB_SIZE_* = workgroup counts; SET_STATE SB_SEL_ENDPOINT 0; RUN_COMPUTE; WAIT slot0; FLUSH_CACHE2; WAIT slot0.
 *
 * Open items carried as parameters so a hardware run can flip them without a rebuild:
 *   fau_count        spec 06 5.3 notes HWR passes bytes/4 where panvk passes 64-bit slots [OPEN]
 *   task increment   RUN_COMPUTE task_increment (1 = HWR)
 * Mesa (MIT) is used as a library (cs_builder.h, genxml packers); the program and recipe are original.
 */
#ifndef MALI_COMPUTE_BUILD_H
#define MALI_COMPUTE_BUILD_H

#ifndef PAN_ARCH
#define PAN_ARCH 11
#endif
#if defined(__has_include)
#  if __has_include("genxml/cs_builder.h")
#    include "genxml/cs_builder.h"
#  else
#    include "cs_builder.h"      /* CI include set lists src/panfrost/genxml directly */
#  endif
#else
#  include "genxml/cs_builder.h"
#endif

#include <stdint.h>
#include <string.h>

namespace mc {

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct Kernel {
    const uint64_t *code;       /* Valhall instruction words */
    uint32_t code_bytes;
    uint64_t preload_mask;      /* bit n = register n preloaded (r48..r63) */
    uint32_t fau_slots;         /* 64-bit slots read by the kernel */
    uint32_t work_regs;         /* 32 or 64 per thread */
    uint32_t wg_size;           /* workgroup size X (Y = Z = 1) */
};

struct JobParams {
    Kernel k;
    uint32_t n_threads;         /* multiple of wg_size */
    uint32_t mul, add;
    uint64_t in_va, out_va;     /* GPU addresses of the data buffers */
    uint32_t fau_count;         /* value of FAU register bits 63:56 (0 = k.fau_slots) */
    uint32_t task_increment;    /* RUN_COMPUTE.task_increment (0 = 1) */
};

struct Layout {
    uint32_t fau_off, code_off, tsd_off, spd_off, cs_off, cs_cap, total;
};

static const uint32_t CS_CAPACITY_BYTES = 2048;   /* 256 instructions: the compute stream is ~30 */

static inline Layout plan(const JobParams &p)
{
    Layout l;
    l.fau_off = 0;
    uint32_t off = (uint32_t)align_up(p.k.fau_slots * 8u, 8);
    l.code_off = (uint32_t)align_up(off, 128);             /* AIR2VH_CODE_ALIGN */
    off = l.code_off + p.k.code_bytes;
    l.tsd_off = (uint32_t)align_up(off, MALI_LOCAL_STORAGE_ALIGN);
    off = l.tsd_off + MALI_LOCAL_STORAGE_LENGTH;
    l.spd_off = (uint32_t)align_up(off, MALI_SHADER_PROGRAM_ALIGN);
    off = l.spd_off + MALI_SHADER_PROGRAM_LENGTH;
    l.cs_off = (uint32_t)align_up(off, 64);                /* MALI_CS_STREAM_ALIGN */
    l.cs_cap = CS_CAPACITY_BYTES;
    l.total = l.cs_off + l.cs_cap;
    return l;
}

struct Built {
    Layout L;
    uint64_t fau_va, code_va, tsd_va, spd_va;
    uint64_t stream_va;
    uint32_t stream_size;       /* bytes, multiple of 8 */
    uint32_t groups;
};

/* Fill FAU, shader, TSD and SPD.  `cpu` is the base of the job buffer (>= plan().total bytes). */
static inline void fill_descriptors(uint8_t *cpu, uint64_t gpu, const JobParams &p, const Layout &l, Built *b)
{
    memset(cpu, 0, l.total);
    /* FAU: slot 0 &in, slot 1 &out, slot 2 {mul, add}; slots are 8 bytes, word 0 = low */
    uint64_t *fau = (uint64_t *)(cpu + l.fau_off);
    if (p.k.fau_slots > 0) fau[0] = p.in_va;
    if (p.k.fau_slots > 1) fau[1] = p.out_va;
    if (p.k.fau_slots > 2) fau[2] = (uint64_t)p.mul | ((uint64_t)p.add << 32);
    memcpy(cpu + l.code_off, p.k.code, p.k.code_bytes);

    /* TSD: no thread-local and no workgroup memory (spec 06 5.3 step 1, HWR "TSD (TODO: local storage)") */
    struct mali_local_storage_packed tsd;
    pan_pack(&tsd, LOCAL_STORAGE, cfg) {
        cfg.tls_size = 0;
        cfg.tls_base_pointer = 0;
        cfg.tls_address_mode = MALI_ADDRESS_MODE_PACKED;
        cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
        cfg.wls_base_pointer = 0;
        cfg.wls_size_base = 0;
        cfg.wls_size_scale = 0;
    }
    memcpy(cpu + l.tsd_off, &tsd, sizeof tsd);

    /* SPD: compute stage; r48..r63 preload mask; 32 or 64 registers per thread (r48..r63 exist only in 64 mode) */
    struct mali_shader_program_packed spd;
    pan_pack(&spd, SHADER_PROGRAM, cfg) {
        cfg.stage = MALI_SHADER_STAGE_COMPUTE;
        cfg.preload.r48_r63 = (uint32_t)(p.k.preload_mask >> 48);
        cfg.suppress_nan = false;
        cfg.flush_to_zero_mode = MALI_FLUSH_TO_ZERO_MODE_PRESERVE_SUBNORMALS;
        cfg.suppress_inf = false;
        cfg.shader_contains_jump_ex = false;
        cfg.register_allocation = p.k.work_regs <= 32 ? MALI_SHADER_REGISTER_ALLOCATION_32_PER_THREAD
                                                       : MALI_SHADER_REGISTER_ALLOCATION_64_PER_THREAD;
        cfg.binary = gpu + l.code_off;
    }
    memcpy(cpu + l.spd_off, &spd, sizeof spd);

    b->L = l;
    b->fau_va = gpu + l.fau_off;
    b->code_va = gpu + l.code_off;
    b->tsd_va = gpu + l.tsd_off;
    b->spd_va = gpu + l.spd_off;
    b->stream_va = gpu + l.cs_off;
    b->groups = p.n_threads / p.k.wg_size;
}

static inline cs_builder_conf user_conf()
{
    cs_builder_conf conf = {};
    conf.nr_registers = 96;                   /* CS0 features 0x7105f [spec 06 2] */
    conf.nr_kernel_registers = 4;             /* r92..r95 belong to the ring wrapper [spec 06 3.4] */
    conf.compute_ep_limit = 0;                /* must be 0 before v12 */
    conf.ls_sb_slot = 0;
    return conf;
}

/* The compute CS stream.  Returns false if the builder ran out of space or flagged an error. */
static inline bool emit_compute_stream(uint8_t *cpu, uint64_t gpu, const JobParams &p, Built *b)
{
    Layout l = plan(p);
    fill_descriptors(cpu, gpu, p, l, b);
    cs_builder_conf conf = user_conf();
    cs_buffer buf = {};
    buf.cpu = (uint64_t *)(cpu + l.cs_off);
    buf.gpu = gpu + l.cs_off;
    buf.capacity = l.cs_cap / 8;
    cs_builder cb;
    cs_builder_init(&cb, &conf, buf);
    cs_builder *c = &cb;

    uint32_t fau_count = p.fau_count ? p.fau_count : p.k.fau_slots;
    cs_req_res(c, CS_COMPUTE_RES);
    cs_move64_to(c, cs_sr_reg64(c, COMPUTE, SRT_0), 0);
    cs_move64_to(c, cs_sr_reg64(c, COMPUTE, FAU_0), b->fau_va | ((uint64_t)fau_count << 56));
    cs_move64_to(c, cs_sr_reg64(c, COMPUTE, SPD_0), b->spd_va);
    cs_move64_to(c, cs_sr_reg64(c, COMPUTE, TSD_0), b->tsd_va);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET), 0);
    struct mali_compute_size_workgroup_packed wg;
    pan_pack(&wg, COMPUTE_SIZE_WORKGROUP, cfg) {
        cfg.workgroup_size_x = p.k.wg_size;
        cfg.workgroup_size_y = 1;
        cfg.workgroup_size_z = 1;
        cfg.allow_merging_workgroups = true;
    }
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, WG_SIZE), wg.opaque[0]);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_OFFSET_X), 0);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_OFFSET_Y), 0);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_OFFSET_Z), 0);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_SIZE_X), b->groups);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_SIZE_Y), 1);
    cs_move32_to(c, cs_sr_reg32(c, COMPUTE, JOB_SIZE_Z), 1);
    cs_select_endpoint_sb(c, 0);
    cs_run_compute(c, p.task_increment ? p.task_increment : 1, MALI_TASK_AXIS_X, cs_shader_res_sel(0, 0, 0, 0));
    cs_wait_slot(c, 0);
    cs_move32_to(c, cs_reg32(c, 0), 0);
    cs_flush_caches(c, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_OTHER_FLUSH_MODE_NONE, cs_reg32(c, 0), cs_defer(0, 0));
    cs_wait_slot(c, 0);

    bool ok = cs_is_valid(c);
    cs_end(c);
    b->stream_size = ok ? cs_root_chunk_size(c) : 0;
    cs_builder_fini(c);
    return ok && b->stream_size && (b->stream_size % 8) == 0;
}

/* A stream with NO shader: stores two magic words and a GPU timestamp to `data_va`, like husky_gpu.c's
 * first job but as a user chunk.  Needs only the top G3D power domain, not the shader cores (R1), so it
 * isolates the submission path (K/S shmem, SUBMIT_CS, CALL wrapper, completion) from shader execution. */
static const uint32_t STORE_MAGIC0 = 0x600DCAFEu;
static const uint32_t STORE_MAGIC1 = 0xB16B00B5u;

static inline bool emit_store_stream(uint8_t *cpu, uint64_t gpu, uint32_t cs_off, uint64_t data_va, Built *b)
{
    cs_builder_conf conf = user_conf();
    cs_buffer buf = {};
    buf.cpu = (uint64_t *)(cpu + cs_off);
    buf.gpu = gpu + cs_off;
    buf.capacity = CS_CAPACITY_BYTES / 8;
    cs_builder cb;
    cs_builder_init(&cb, &conf, buf);
    cs_builder *c = &cb;
    cs_move64_to(c, cs_reg64(c, 0), data_va);
    cs_move32_to(c, cs_reg32(c, 2), STORE_MAGIC0);
    cs_move32_to(c, cs_reg32(c, 3), STORE_MAGIC1);
    cs_store32(c, cs_reg32(c, 2), cs_reg64(c, 0), 0);
    cs_store32(c, cs_reg32(c, 3), cs_reg64(c, 0), 4);
    cs_store_state(c, cs_reg64(c, 0), 8, MALI_CS_STATE_TIMESTAMP, cs_now());
    cs_wait_slot(c, 0);
    cs_move32_to(c, cs_reg32(c, 0), 0);
    cs_flush_caches(c, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_OTHER_FLUSH_MODE_NONE, cs_reg32(c, 0), cs_defer(0, 0));
    cs_wait_slot(c, 0);
    bool ok = cs_is_valid(c);
    cs_end(c);
    b->stream_va = gpu + cs_off;
    b->stream_size = ok ? cs_root_chunk_size(c) : 0;
    cs_builder_fini(c);
    return ok && b->stream_size && (b->stream_size % 8) == 0;
}

}  /* namespace mc */

#endif /* MALI_COMPUTE_BUILD_H */
