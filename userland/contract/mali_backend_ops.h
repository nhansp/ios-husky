/*
 * mali_backend_ops.h - the kmod-style interface the kernel Mali backend exposes
 * -------------------------------------------------------------------------
 * Intended repo path: kernel/iokit/Families/IOGPU/mali/mali_backend_ops.h
 *
 * The internal contract between the IOGPUFamily re-implementation (device,
 * resource, memory-map, command-queue, event-machine objects) and the
 * Mali-G715 (Valhall CSF, arch 11.8) hardware backend. Everything the family
 * hooks (spec 03) need from hardware is expressed here as a flat ops table,
 * modeled on Mesa's pan_kmod device/VM/BO/queue surface and grown from the
 * WORKING bring-up code at
 *   artifacts/xnu-12377/bsd/dev/husky_gpu.c  (cited HG:<line>)
 * which already boots the CSF firmware and ran a first CSG0/CS0 job on real
 * hardware. This header EXTENDS that code; it does not regress it.
 *
 * ORIGINAL work. pan_kmod (pan_kmod.h), panthor UAPI (panthor_drm.h),
 * panthor_sched.c and kbase were studied as references; nothing is copied.
 * Reference citations use the spec prefixes (spec 06 citation key):
 *   M:   mesa/src/panfrost     PT:  panthor driver     KB:  mali_kbase
 *   CSR: panthor_sched.c etc.  HG:  husky_gpu.c
 *
 * HARDWARE GROUND TRUTH (husky, read live, spec 06 section 2):
 *   GPU_ID 0xb8a21020 (arch 11.8, product major 2, r1p2); 7 shader cores
 *   SHADER_PRESENT 0x01110055 (core-id range 25); TILER/L2 present; AS_PRESENT
 *   0xff (AS0 = MCU, 7 left for contexts); CSF GLB 2.8.0, 8 CSGs, 8 CS/CSG;
 *   CS0 features 0x7105f (96 work regs, 16 scoreboard slots); non-coherent.
 *
 * RISK R1 (MUST be resolved before any RUN_COMPUTE/RUN_FRAGMENT):
 *   husky_gpu.c powers ONLY the top G3D domain (pd_g3d). The shader cores
 *   (pd_embedded_g3d) and the G3D PLLs (CMU_G3D) are NOT powered. The first
 *   job needed none (stores + sync only). Real jobs need cores. The backend
 *   MUST implement mali_backend_ops::power_shader_cores, porting the sequence
 *   from Linux exynos-pd_el3.c + the zuma PMU DT (pd_embedded_g3d at PMU
 *   0x15461C80, child of pd_g3d) before dispatching work [spec 03 4.1 #1,
 *   spec 03 section 8 "Gap", spec 06 section 2, open question Q8].
 */
#ifndef MALI_BACKEND_OPS_H
#define MALI_BACKEND_OPS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * 0. Opaque handles. The family holds these; only the backend derefs them.
 * ===================================================================== */
typedef struct mali_backend  mali_backend_t;   /* one per accelerator */
typedef struct mali_vm       mali_vm_t;         /* one GPU address space per IOGPUTask */
typedef struct mali_bo       mali_bo_t;         /* a GPU buffer object (resource backing) */
typedef struct mali_queue    mali_queue_t;      /* one CSG+CS group (one IOGPUCommandQueue) */
typedef struct mali_syncobj  mali_syncobj_t;    /* a 64-bit CS sync object */
typedef struct mali_tiler_heap mali_tiler_heap_t;

typedef uint64_t mali_gpu_va_t;                 /* 48-bit GPU virtual address */
typedef int      mali_status_t;                 /* 0 = ok, else an errno-style code */

/* ===================================================================== *
 * 1. Device properties - the subset Mesa library code consumes
 *   (struct pan_kmod_dev_props, M:lib/kmod/pan_kmod.h:194-275; spec 06 5.1).
 *   Filled from GPU_CONTROL register reads [HG:136-161, HG:24-27].
 * ===================================================================== */
typedef struct mali_dev_props {
    uint64_t gpu_id;                 /* GPU_ID 0x00  (0xb8a21020) */
    uint32_t gpu_variant;            /* CORE_FEATURES & 0xff */
    uint64_t gpu_features;           /* GPU_FEATURES 0x60 */
    uint64_t shader_present;         /* SHADER_PRESENT 0x100/0x104 (0x01110055) */
    uint32_t tiler_features;         /* TILER_FEATURES 0x0c (0x809) */
    uint32_t mem_features;           /* MEM_FEATURES 0x10 (0x301, 4 L2 slices) */
    uint32_t mmu_features;           /* MMU_FEATURES 0x14 (0x2830: 48 VA / 40 PA bits) */
    uint32_t l2_features;            /* L2_FEATURES 0x04 */
    uint32_t as_present;             /* AS_PRESENT 0x18 (0xff) */
    uint32_t csf_id;                 /* CSF_ID 0x1c */
    uint32_t texture_features[4];    /* TEXTURE_FEATURES0..3 0xB0+4n */
    uint32_t max_threads_per_core;   /* THREAD_MAX_THREADS 0xA0 */
    uint32_t max_threads_per_wg;     /* THREAD_MAX_WORKGROUP_SIZE 0xA4 */
    uint32_t max_tasks_per_core;     /* THREAD_FEATURES >> 24 */
    uint32_t num_registers_per_core; /* THREAD_FEATURES & 0x3fffff */
    uint32_t core_count;             /* popcount(shader_present) = 7 */
    uint32_t core_id_range;          /* last-set-bit of shader_present + 1 = 25 (WLS sizing) */
    uint32_t cs_reg_count;           /* CS0 features & 0xff + 1 = 96 */
    uint32_t cs_scoreboard_slots;    /* (CS0 features >> 8) & 0xff = 16 */
    uint32_t glb_version;            /* GLB_VERSION (2.8.0) */
    uint32_t num_csg;                /* 8 */
    uint32_t num_cs_per_csg;         /* 8 */
    uint32_t group_suspend_size;     /* CSG GROUP_SUSPEND_SIZE (0x6700) */
    uint64_t pgsize_bitmap;          /* 4 KiB granule only [HG:229] */
    bool     is_io_coherent;         /* false [claim C16-noncoherent; HG:902] */
} mali_dev_props_t;

/* ===================================================================== *
 * 2. GPU MMU / page-table contract [HG:208-209, HG:504-542; claim C16]
 *   48-bit VA, 40-bit PA, 4 KiB granule, 4 levels. XNU PAGE_SIZE is 16 KiB
 *   here, so the GPU granule is tracked separately.
 * ===================================================================== */
#define MALI_GPU_PAGE_SHIFT     12
#define MALI_GPU_PAGE_SIZE      0x1000u
#define MALI_GPU_PAGE_MASK      (MALI_GPU_PAGE_SIZE - 1)

/* AS (address space) register programming [HG:188-209, HG:627-640]. */
#define MALI_AS_MEMATTR_VALUE   0x4C4C4C4C8C4C8F4CULL /* idx0 non-cacheable, idx1 write-back */
#define MALI_AS_TRANSCFG_VALUE  0x420001C6ULL
#define MALI_AS_CMD_UPDATE      1u
/* AttrIndx selectors inside a leaf PTE, matching AS_MEMATTR_VALUE. */
#define MALI_ATTR_NONCACHEABLE  0u   /* outer-shareable, for CPU-written GPU data */
#define MALI_ATTR_WRITEBACK     1u   /* inner-shareable */

/*
 * Leaf PTE bit layout the backend writes [HG:504-542, leaf_attrs_for()]:
 *   bits 1:0 = 3 (valid page), AF (bit10), nG (bit11), AP[1] (bit6),
 *   read-only via AP[2] (bit7) when !writable, XN (bits 53,54) when !exec,
 *   AttrIndx (bit2) + SH (bits 9:8) chosen by cacheability.
 * AS1 (job address space) shares the MCU's AS0 root today [HG:627-640]; the
 * backend generalises to one root per mali_vm.
 */
typedef struct mali_pte_attrs {
    bool writable;
    bool executable;
    uint32_t attr_index;     /* MALI_ATTR_* */
} mali_pte_attrs_t;

/* ===================================================================== *
 * 3. Buffer-object (resource backing) flags [pan_kmod supported_bo_flags].
 *   The GPU is non-coherent: CPU-written pages that the GPU reads MUST be
 *   cleaned to DRAM, or mapped non-cacheable [claim C16; HG:451-468, HG:848].
 * ===================================================================== */
enum mali_bo_flag {
    MALI_BO_GPU_READ     = (1u << 0),
    MALI_BO_GPU_WRITE    = (1u << 1),
    MALI_BO_GPU_EXEC     = (1u << 2),  /* shader code: must sit in the first 4 GiB (spec 06 D7) */
    MALI_BO_CPU_MAP      = (1u << 3),  /* client-visible CPU mapping requested */
    MALI_BO_NONCACHEABLE = (1u << 4),  /* map write-combined/NC (ml_io_map_wcomb, HG:621) */
    MALI_BO_FIXED_VA     = (1u << 5),  /* honour the requested GPU VA (sel 8 args +0x20) */
    MALI_BO_ZERO         = (1u << 6),  /* zero-filled on allocation */
    MALI_BO_GPU_CACHED   = (1u << 7),  /* GPU side write-back (L2) even when the CPU view is NC; never for
                                        * memory the CPU polls while the GPU runs (sync objects) */
};

/* ===================================================================== *
 * 4. CSF queue contract - the three layers that already work [spec 06 section 3].
 * ===================================================================== */

/*
 * Layer C: user command-stream instruction encodings (opcode in bits 63:56),
 * exactly as husky_gpu.c emits them [HG:329-342]. The Mali plug-in's cs_builder
 * output is a byte stream of these; the kernel validates and CALLs it.
 */
#define MALI_CS_MOVE48(r, imm) \
    ((1ULL << 56) | ((uint64_t)(r) << 48) | ((uint64_t)(imm) & 0xffffffffffffULL))
#define MALI_CS_MOVE32(r, imm) \
    ((2ULL << 56) | ((uint64_t)(r) << 48) | (uint32_t)(imm))
#define MALI_CS_WAIT(mask)          ((3ULL << 56) | ((uint64_t)(mask) << 16))
#define MALI_CS_STORE_MULTIPLE(src, addr, mask, off) \
    ((21ULL << 56) | ((uint64_t)(src) << 48) | ((uint64_t)(addr) << 40) | \
     ((uint64_t)(mask) << 16) | (uint16_t)(off))
#define MALI_CS_STORE_STATE_TS(addr, off) \
    ((40ULL << 56) | ((uint64_t)(addr) << 40) | (uint16_t)(off))
#define MALI_CS_FLUSH_CACHE2(id_reg) /* L2+LSC clean-invalidate, other invalidate */ \
    ((36ULL << 56) | ((uint64_t)(id_reg) << 40) | 0x233ULL)
#define MALI_CS_SYNC_ADD64(addr, val) /* system scope, propagate errors */ \
    ((51ULL << 56) | ((uint64_t)(addr) << 40) | ((uint64_t)(val) << 32) | 1ULL)
#define MALI_CS_CALL(addr_reg, len_reg) \
    ((32ULL << 56) | ((uint64_t)(addr_reg) << 40) | ((uint64_t)(len_reg) << 32))
#define MALI_CS_ERROR_BARRIER       (47ULL << 56)

/*
 * Register ABI (arch 11) [spec 06 3.4]: 96 work regs, last 4 kernel-reserved,
 * so user CS may use r0..r91. The kernel ring wrapper's own registers are
 * r92:93 (address) and r94 (value / flush id) [HG:611]. cs_builder chunk-link
 * registers overlap these on purpose (consumed by CALL before user code runs).
 */
#define MALI_CS_KERNEL_RESERVED_REGS  4u
#define MALI_CS_USER_REG_MAX          91u

/*
 * Layer B: the per-job kernel ring wrapper (panthor queue_run_job, spec 06 3.2).
 * husky_gpu.c already runs the FLUSH_CACHE2 + WAIT + SYNC_ADD64 + ERROR_BARRIER
 * tail [HG:642-655]; the only new instruction per job is CALL of the user chunk.
 * The user stream must be 64-byte aligned, size a multiple of 8
 * [PT:panthor_drm.h:973,985]. latest_flush = 0 forces an unconditional flush.
 */
#define MALI_CS_STREAM_ALIGN        64u
#define MALI_CS_LATEST_FLUSH_FORCE  0u

/* The Panthor submit tuple the kernel receives per command buffer [spec 06 D3]. */
typedef struct mali_submit_desc {
    uint32_t      queue_index;       /* CS slot within the group (0 for compute-only v0) */
    mali_gpu_va_t stream_addr;       /* cs_root_chunk_gpu_addr (64-byte aligned) */
    uint32_t      stream_size;       /* cs_end() size (multiple of 8) */
    uint32_t      latest_flush;      /* 0 to force the leading flush */
    /* wait/signal: 64-bit CS sync objects, resolved from the segment list */
    const mali_syncobj_t *const *waits;  uint32_t n_waits;
    const mali_syncobj_t *const *signals; uint32_t n_signals;
} mali_submit_desc_t;

/*
 * MALI vendor kernel-command types (IOGPU kernel commands with type >= 0x13,
 * iogpu_uc_abi.h kIOGPUKCmd_VendorBase). These travel in shmem K and are
 * decoded by MaliCommandQueue::processSegmentKernelCommand [spec 03 5.6,
 * spec 06 section 3]. Relocations in SUBMIT_CS are validated against the
 * segment-list resource set before the CALL. Body layouts are the plug-in
 * <-> kernel private agreement; these ids are our choice (not Apple's).
 */
enum mali_kcmd_type {
    MALI_KCMD_FENCE     = 0x13,  /* stamp write: CS STORE of the stamp + flush + sync */
    MALI_KCMD_FILL      = 0x14,  /* fill a BO range (libpan panlib_fill) */
    MALI_KCMD_SUBMIT_CS = 0x15,  /* point the CS ring at a user stream chunk; body:
                                  * { mali_gpu_va_t stream; u32 size; u32 flush_id;
                                  *   u32 n_wait; u32 n_signal; <sync ids...> } */
    MALI_KCMD_HEAP_OP   = 0x16,  /* tiler-heap set/operation for render passes */
};

/* ===================================================================== *
 * 5. The backend ops table. The family calls these; the backend implements
 *   them on top of husky_gpu.c's proven sequences. Return 0 on success.
 * ===================================================================== */
typedef struct mali_backend_ops {
    /* --- accelerator lifecycle (IOGPU::configureDevice / teardownDevice) --- */
    /* probe + power pd_g3d, open the G3D S2MPU, reset, L2 on, boot the CSF
     * firmware on the MCU. This is husky_gpu_probe/husky_gpu_mcu_boot
     * [HG:1003, HG:726, spec 03 4.1 #1]. Returns a mali_backend_t. */
    mali_status_t (*configure_device)(const void *provider, mali_backend_t **out);
    void          (*teardown_device)(mali_backend_t *); /* quiesce MCU, power off (reverse) */
    void          (*get_props)(mali_backend_t *, mali_dev_props_t *out);

    /*
     * RISK R1: power the shader cores. MUST be called before the first
     * RUN_COMPUTE/RUN_FRAGMENT. Ports pd_embedded_g3d power-on + G3D PLL bring-up
     * from exynos-pd_el3.c + zuma PMU DT. Not needed for stores-only jobs.
     * [spec 03 4.1, spec 06 section 2, open question Q8]
     */
    mali_status_t (*power_shader_cores)(mali_backend_t *, bool on);

    /* --- stamp memory (IOGPU getStampMemory, spec 03 4.3) --- */
    /* return the kernel/user-shared, page-aligned stamp page descriptor and the
     * client's offset into it; the CS writes 32-bit stamp values to it. */
    mali_status_t (*get_stamp_memory)(mali_backend_t *, void **desc_out, uint32_t *offset_out);

    /* --- VM: one GPU address space per IOGPUTask (spec 03 5.3) --- */
    mali_status_t (*vm_create)(mali_backend_t *, mali_vm_t **out);
    void          (*vm_destroy)(mali_vm_t *);
    /* assign/evict a hardware AS slot (7 for contexts; LRU when > 7 clients) and
     * run AS_COMMAND UPDATE (lock/flush-PT/unlock) [HG:627-640, spec 06 5.2]. */
    mali_status_t (*vm_bind_as_slot)(mali_vm_t *, int *as_slot_out);
    void          (*vm_flush_as)(mali_vm_t *);

    /* --- BO: resource backing (IOGPUSysMemory + MaliMemoryMap, spec 03 5.4) --- */
    mali_status_t (*bo_alloc)(mali_vm_t *, uint64_t size, uint32_t flags,
                              mali_gpu_va_t fixed_va, mali_bo_t **out);
    /* wrap client-supplied pages (sel 8 type 0x80) or an IOSurface plane (0x82). */
    mali_status_t (*bo_import_pages)(mali_vm_t *, const uint64_t *phys, uint32_t n_pages,
                                     uint32_t flags, mali_bo_t **out);
    void          (*bo_free)(mali_bo_t *);
    mali_gpu_va_t (*bo_gpu_va)(const mali_bo_t *);
    void *        (*bo_cpu_map)(const mali_bo_t *);       /* NULL if not CPU-mapped */
    /* MaliMemoryMap::commitIntoGPUPageTable / releaseFromGPUPageTable (pure). */
    mali_status_t (*bo_map)(mali_bo_t *, mali_gpu_va_t va, const mali_pte_attrs_t *);
    void          (*bo_unmap)(mali_bo_t *);

    /* --- cache maintenance (non-coherent GPU) [claim C16; HG:457-468] --- */
    /* clean+invalidate a CPU-written range to DRAM before a GPU read; call on
     * segment-list prepare and map commit [spec 03 5.4]. */
    void          (*dcache_flush)(const void *cpu_addr, uint64_t len);

    /* --- queue: one CSG + CS group per IOGPUCommandQueue (spec 03 5.6) --- */
    /* program CSG/CS slots the way husky_gpu_first_job does [HG:585-724]:
     * ring buffer (64 KiB), AS slot, suspend buffer, EP request, doorbells. */
    mali_status_t (*queue_create)(mali_vm_t *, uint32_t cs_mask, mali_queue_t **out);
    void          (*queue_destroy)(mali_queue_t *);
    /* write the Layer-B ring wrapper for one command buffer and ring the
     * doorbell; the completion is read from the per-queue sync object. */
    mali_status_t (*queue_submit)(mali_queue_t *, const mali_submit_desc_t *);
    /* v0 synchronous completion: poll the sync object / stamp [HG:711-716];
     * v1 replaces this with the JOB IRQ + signalStampsUpdated path. */
    mali_status_t (*queue_wait_idle)(mali_queue_t *, uint64_t timeout_us);

    /* --- syncobj: 64-bit CS sync object {seqno,error,pad} (spec 06 3.5) --- */
    mali_status_t (*syncobj_create)(mali_vm_t *, mali_syncobj_t **out);
    void          (*syncobj_destroy)(mali_syncobj_t *);
    mali_gpu_va_t (*syncobj_gpu_va)(const mali_syncobj_t *);
    uint64_t      (*syncobj_read)(const mali_syncobj_t *);

    /* --- tiler heap: FW-owned, for render passes (spec 06 5.4.5) --- */
    /* chunk size page-aligned in [128 KiB, 8 MiB]; returns the triple the CS
     * needs: {handle, ctx_gpu_va, first_chunk_gpu_va}. Kernel half of
     * CS_TILER_OOM lives here (group_process_tiler_oom). */
    mali_status_t (*tiler_heap_create)(mali_vm_t *, uint32_t chunk_size,
                                       uint32_t initial_chunks, uint32_t max_chunks,
                                       mali_tiler_heap_t **out,
                                       mali_gpu_va_t *ctx_va_out,
                                       mali_gpu_va_t *first_chunk_va_out);
    void          (*tiler_heap_destroy)(mali_tiler_heap_t *);
} mali_backend_ops_t;

/* The single backend instance the family binds to at IOGPU::start. */
extern const mali_backend_ops_t mali_backend_ops;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* MALI_BACKEND_OPS_H */
