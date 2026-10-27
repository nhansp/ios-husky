/*
 * iogpu_internal.h - Internal shared definitions for IOGPUFamily.
 * -------------------------------------------------------------------------
 * Target repo path: kernel/iokit/Families/IOGPU/iogpu_internal.h
 *
 * Implements the verified iOS 26.5 IOKit user-client ABI against Apple's
 * IOGPUFamily 130.14 / IOGPU.framework [spec 01 section 0, claim A17-kernel-version].
 *
 * All selector numbers, offsets, and struct layouts cite:
 *   - ../refs/gpu-spec/01-iogpu-userclient-abi.md (spec 01)
 *   - ../refs/gpu-spec/02-iogpu-caller-flow.md (spec 02)
 *   - ../refs/gpu-spec/03-gpu-driver-hooks.md (spec 03)
 *   - ../refs/kernel/husky_gpu.c (proven Mali-G715 hardware pipeline)
 */
#ifndef IOGPU_INTERNAL_H
#define IOGPU_INTERNAL_H

#ifdef __cplusplus
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IOLocks.h>
#endif

#include "iogpu_uc_abi.h"
#include "mali_backend_ops.h"

/*
 * IOGPUConfig: accelerator configuration block.
 * In iOS 26.5 / IOGPUFamily 130.14, this block lives at accelerator offset
 * accel+0x1e8 [spec 01 lines 19, 304-315; claim A24-config-base].
 */
struct IOGPUConfig {
	char      accelName[0x40];    /* +0x1e8 -> selector 1 GET_NAME (0x40 B) [spec 01 line 320] */
	uint32_t  configBits;         /* +0x1f0 bit6 supportPriorityBand [spec 01 line 304] */
	uint32_t  debugBits;          /* +0x1fc boot-arg debug controls */
	uint32_t  deviceBits;         /* +0x1f8 [spec 01 line 306] */
	uint64_t  videoRam;           /* +0x208 0 = unified memory [spec 01 line 308] */
	uint32_t  cfg0;               /* +0x22c -> selector 2 GET_STAMP_CONFIG +0x08 [spec 01 line 286] */
	uint32_t  cfg1;               /* +0x230 -> selector 2 GET_STAMP_CONFIG +0x0c [spec 01 line 287] */
	uint32_t  privateSize;        /* +0x234 -> selector 2 GET_STAMP_CONFIG +0x10 [spec 01 line 288] */
	uint32_t  cmdBufArgsSize;     /* +0x268 submit arg size (0x30) [spec 01 lines 543, 548] */
	uint32_t  queueArgsSize;      /* +0x278 sel 6 struct-in size (0x408) [spec 01 line 423] */
	uint32_t  numEventChannels;   /* event machine stamp slots [spec 01 line 290] */
	uint64_t  sharedMemorySize;   /* GET_DEVICE_INFO +0x20 [spec 01 line 311] */
	uint64_t  maxResourceSize;    /* GET_DEVICE_INFO +0x28 (1 GiB) [spec 01 line 312] */
};

/*
 * Vendor extension definitions for Mali-G715 plug-in <-> kernel contract
 * [spec 01 section 9, MaliPluginABI.h lines 70-155].
 */

/* D-1: Device properties published in the IORegistry as CFData 'MaliDevProps' */
#define MALI_DEV_PROPS_BLOB_MAGIC   0x4d414c49u      /* 'MALI' */
#define MALI_DEV_PROPS_BLOB_VERSION 1u
struct mali_dev_props_blob {
	uint32_t magic;              /* MALI_DEV_PROPS_BLOB_MAGIC */
	uint32_t version;            /* MALI_DEV_PROPS_BLOB_VERSION */
	uint32_t struct_size;        /* sizeof(mali_dev_props_t) */
	uint32_t pad;
	mali_dev_props_t props;
};

/* D-2: Resource args vendor tail (0x10 bytes after generic 0x58 = 0x68 total) */
#define MALI_RES_TAIL_MAGIC         0x4d424f31u      /* 'MBO1' [spec 01 line 459, MaliPluginABI.h line 83] */
struct mali_new_resource_tail {
	uint32_t magic;              /* MALI_RES_TAIL_MAGIC */
	uint32_t bo_flags;           /* enum mali_bo_flag (mali_backend_ops.h) */
	uint32_t mtl_pixel_format;   /* informational */
	uint32_t reserved;
};

typedef struct mali_new_resource_args {
	iogpu_new_resource_args_t generic;  /* generic 0x58 [spec 01 line 457] */
	struct mali_new_resource_tail tail; /* vendor tail 0x10 [MaliPluginABI.h line 92] */
} mali_new_resource_args_t;

/* D-3: Vendor kernel commands (shmem K, type >= 0x13) [spec 01 line 451, mali_backend_ops.h lines 197-211] */
struct mali_kcmd_submit_cs {
	iogpu_kcmd_header_t header;  /* type MALI_KCMD_SUBMIT_CS (0x15), size >= 0x20 */
	uint64_t stream;             /* GPU VA, 64-byte aligned */
	uint32_t size;               /* byte size, multiple of 8 */
	uint32_t flush_id;           /* 0 = unconditional flush */
	uint32_t n_wait;
	uint32_t n_signal;
	/* uint32_t sync_ids[n_wait + n_signal]; */
};

enum mali_heap_op {
	MALI_HEAP_OP_CREATE  = 1,
	MALI_HEAP_OP_DESTROY = 2
};

struct mali_kcmd_heap_op {
	iogpu_kcmd_header_t header;  /* type MALI_KCMD_HEAP_OP (0x16), size 0x28 */
	uint32_t op;                 /* enum mali_heap_op */
	uint32_t chunk_size;         /* e.g. 2 MiB */
	uint32_t initial_chunks;
	uint32_t max_chunks;
	uint64_t qstate_gpu_va;
	uint64_t qstate_resource_id;
};

struct mali_heap_result {
	uint64_t ctx_gpu_va;
	uint64_t first_chunk_gpu_va;
	uint32_t state;
	uint32_t error;
};

struct mali_kcmd_fill {
	iogpu_kcmd_header_t header;  /* type MALI_KCMD_FILL (0x14), size 0x20 */
	uint32_t resource_id;
	uint32_t pattern32;
	uint64_t offset;
	uint64_t bytes;
};

#ifdef __cplusplus
/* Forward declarations */
class IOAcceleratorES;
class IOGPU;
class IOGPUDevice;
class IOGPUDeviceUserClient;
class IOGPUResource;
class IOGPUSysMemory;
class IOGPUMemoryMap;
class IOGPUTask;
class IOGPUCommandQueue;
class IOGPUEventMachine;
class IOGPUNotificationQueue;
class IOGPUDeviceShmem;
class MaliAccelerator;
#endif

#endif /* IOGPU_INTERNAL_H */
