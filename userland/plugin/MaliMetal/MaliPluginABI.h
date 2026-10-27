/*
 * MaliPluginABI.h - the plug-in <-> kernel private agreement
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliPluginABI.h
 * Plain C; the kernel's MaliAccelerator/MaliCommandQueue may include it too.
 *
 * The three contract headers fix the IOGPU wire ABI (iogpu_uc_abi.h), the kernel
 * backend surface (mali_backend_ops.h) and the compiler API (air2vh_api.h). They
 * leave four things to "the plug-in <-> kernel private agreement" (iogpu_uc_abi.h
 * kIOGPUKCmd_VendorBase, mali_backend_ops.h section 4 "Body layouts are ... our
 * choice"). This file is that agreement. Each item says what the kernel module
 * has to do for the plug-in to work; the same list is in the module's open
 * questions.
 *
 * D-1  ONE COMMAND STREAM PER QUEUE. iogpu_uc_abi.h/mali_backend_ops.h give the
 *      MALI_KCMD_SUBMIT_CS body no queue index, so every IOGPU command queue runs on
 *      CS0 of its group (mali_submit_desc.queue_index = 0). Vertex/tiler work and the
 *      fragment job of a pass run in order in that CS with scoreboard waits. The
 *      three-queue split of panvk (S06 5.4) is a later optimisation that needs a
 *      queue index in the body (suggested: flush_id bits 31:24).
 *
 * D-2  VENDOR RESOURCE-ARGS TAIL. iogpu_new_resource_args_t is the generic 0x58
 *      bytes; the plug-in appends a 16-byte vendor tail (mali_new_resource_tail)
 *      carrying MALI_BO_* flags, notably MALI_BO_GPU_EXEC for shader code, which
 *      must sit inside one 4 GiB window (S07 6.13, S06 D7). The kernel's sel 8
 *      handler may read structureInput bytes [0x58, 0x68) when structureInputSize
 *      >= 0x68 and must ignore them otherwise (the IOGPU framework only enforces
 *      >= 0x58, spec 02 section 6.1). The command-queue args are the generic 0x408
 *      bytes with NO vendor tail (the framework requires size == accel+0x278, whose
 *      default is the generic size).
 *
 * D-3  TILER HEAP. A Metal queue needs a firmware tiler heap before its first render
 *      pass. The plug-in allocates a queue-state buffer (MALI_QSTATE_SIZE, see
 *      mali_core.h), then sends MALI_KCMD_HEAP_OP(CREATE) in the first command buffer
 *      that renders. The kernel creates the heap through mali_backend_ops
 *      tiler_heap_create, WRITES the TILER_HEAP descriptor {size, base, bottom, top}
 *      at queue-state+0 and a struct mali_heap_result at queue-state+0x80, and only
 *      then runs the SUBMIT_CS of that command buffer. The CS loads heap_ctx from the
 *      result block and executes HEAP_SET. The kernel half of the tiler OOM handshake
 *      (group_process_tiler_oom) is required.
 *
 * D-4  FENCE. MALI_KCMD_FENCE (8-byte record, no body) is the last record of every
 *      command buffer: when every SUBMIT_CS before it has retired the kernel completes
 *      the command buffer (notification entry, completedRef) - the same shape as
 *      SnapAdreno's synchronous fence transport (spec 03 section 8 item 9).
 *
 * Registry properties the kernel publishes on the accelerator node (all optional
 * with documented fallbacks):
 *   MetalPluginName       "MaliMetal"            (contract iogpu_uc_abi.h)
 *   MetalPluginClassName  "MaliMTLDevice"
 *   MaliDevProps          CFData: struct mali_dev_props_blob (below). Fallback: the husky
 *                         ground truth of mali_backend_ops.h. THE THREAD-LIMIT FIELDS
 *                         MATTER: they size thread-local storage.
 */
#ifndef MALI_PLUGIN_ABI_H
#define MALI_PLUGIN_ABI_H

#include <stdint.h>
#include "iogpu_uc_abi.h"
#include "mali_backend_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_PROP_PLUGIN_NAME       "MetalPluginName"
#define MALI_PROP_PLUGIN_CLASS      "MetalPluginClassName"
#define MALI_PROP_DEV_PROPS         "MaliDevProps"

#define MALI_DEV_PROPS_BLOB_MAGIC   0x4d414c49u      /* 'MALI' */
#define MALI_DEV_PROPS_BLOB_VERSION 1u
struct mali_dev_props_blob {
    uint32_t magic;              /* MALI_DEV_PROPS_BLOB_MAGIC                   */
    uint32_t version;            /* MALI_DEV_PROPS_BLOB_VERSION                 */
    uint32_t struct_size;        /* sizeof(mali_dev_props_t) of the writer      */
    uint32_t _pad;
    mali_dev_props_t props;
};

/* ------------------------------------------------------------------ *
 * D-2: resource args with the vendor tail
 * ------------------------------------------------------------------ */
#define MALI_RES_TAIL_MAGIC         0x4d424f31u      /* 'MBO1' */
struct mali_new_resource_tail {
    uint32_t magic;              /* MALI_RES_TAIL_MAGIC                         */
    uint32_t bo_flags;           /* enum mali_bo_flag (mali_backend_ops.h)       */
    uint32_t mtl_pixel_format;   /* textures: MTLPixelFormat; informational     */
    uint32_t reserved;
};
typedef struct mali_new_resource_args {
    iogpu_new_resource_args_t generic;       /* 0x58, filled by IOGPU.framework  */
    struct mali_new_resource_tail tail;      /* 0x10                             */
} mali_new_resource_args_t;
_Static_assert(sizeof(iogpu_new_resource_args_t) == IOGPU_RESOURCE_ARGS_MIN, "generic resource args are 0x58");
_Static_assert(sizeof(mali_new_resource_args_t) == 0x68, "vendor resource args are 0x68");

/* D-2: queue args have no vendor tail */
_Static_assert(sizeof(iogpu_new_command_queue_args_t) == IOGPU_NEW_QUEUE_ARGS_MIN, "generic queue args are 0x408");
#define MALI_QUEUE_ARGS_SIZE        IOGPU_NEW_QUEUE_ARGS_MIN

/* ------------------------------------------------------------------ *
 * Kernel command bodies (follow iogpu_kcmd_header_t = {type, size})
 * ------------------------------------------------------------------ */

/* MALI_KCMD_FENCE: header only (size 8). */

/* MALI_KCMD_SUBMIT_CS: the body documented in mali_backend_ops.h
 *   { mali_gpu_va_t stream; u32 size; u32 flush_id; u32 n_wait; u32 n_signal; <u32 sync ids...> }
 * v1 always sends n_wait = n_signal = 0 (cross-command-buffer ordering is the in-order queue;
 * MTLEvent/MTLSharedEvent use the generic IOGPU kernel commands). stream is 64-byte aligned,
 * size a multiple of 8. flush_id 0 forces the leading FLUSH_CACHE2 (S06 3.2). */
struct mali_kcmd_submit_cs {
    iogpu_kcmd_header_t header;  /* type MALI_KCMD_SUBMIT_CS, size 0x20            */
    uint64_t stream;
    uint32_t size;
    uint32_t flush_id;
    uint32_t n_wait;
    uint32_t n_signal;
};
_Static_assert(sizeof(struct mali_kcmd_submit_cs) == 0x20, "SUBMIT_CS record size with no syncs");

/* MALI_KCMD_HEAP_OP */
enum mali_heap_op { MALI_HEAP_OP_CREATE = 1, MALI_HEAP_OP_DESTROY = 2 };
#define MALI_HEAP_CHUNK_SIZE        (2u << 20)       /* [MV:panvk_physical_device.c:498-505] */
#define MALI_HEAP_INITIAL_CHUNKS    5u
#define MALI_HEAP_MAX_CHUNKS        64u
struct mali_kcmd_heap_op {
    iogpu_kcmd_header_t header;  /* type MALI_KCMD_HEAP_OP, size 0x28             */
    uint32_t op;                 /* enum mali_heap_op                              */
    uint32_t chunk_size;         /* MALI_HEAP_CHUNK_SIZE: the plug-in sizes the tiler hierarchy for it */
    uint32_t initial_chunks;
    uint32_t max_chunks;
    uint64_t qstate_gpu_va;      /* GPU VA of the queue-state buffer (MALI_QSTATE_*) */
    uint64_t qstate_resource_id; /* IOGPU resource id of that buffer (kernel finds its CPU mapping) */
};
_Static_assert(sizeof(struct mali_kcmd_heap_op) == 0x28, "HEAP_OP record size");

/* Written by the kernel into the queue-state buffer at MALI_QSTATE_RESULT_OFF (0x80) */
struct mali_heap_result {
    uint64_t ctx_gpu_va;         /* firmware heap context, the HEAP_SET operand     */
    uint64_t first_chunk_gpu_va;
    uint32_t state;              /* 1 = created                                     */
    uint32_t error;              /* errno-style, 0 ok                               */
};

/* MALI_KCMD_FILL (defined by the contract, unused by the v1 plug-in: fills run as
 * compute kernels inside the CS so they stay ordered against render passes). */
struct mali_kcmd_fill {
    iogpu_kcmd_header_t header;  /* size 0x20                                       */
    uint32_t resource_id;
    uint32_t pattern32;
    uint64_t offset;
    uint64_t bytes;
};

/* ------------------------------------------------------------------ *
 * Resource sizing constants shared with the kernel's 16 KiB pages
 * ------------------------------------------------------------------ */
#define MALI_PAGE_SIZE              16384u           /* XNU page; GPU pages are 4 KiB (S06 section 11 item 8) */
#define MALI_CMDBUF_ARENA_CHUNK     (1u << 20)       /* per command buffer descriptor/CS arena chunk */

#ifdef __cplusplus
}
#endif
#endif /* MALI_PLUGIN_ABI_H */
