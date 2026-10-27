/*
 * iogpu_uc_abi.h - IOGPU user-client wire ABI (the ONE source of truth)
 * -------------------------------------------------------------------------
 * Intended repo path: kernel/iokit/Families/IOGPU/iogpu_uc_abi.h
 * Shared between the kernel user client (implements) and the Metal plug-in
 * (calls). ORIGINAL work; the GPL/MIT reference trees were studied, never
 * copied.
 *
 * Target: a from-source XNU on the Pixel 8 Pro (husky), driving a MATCHED
 * iOS 26.5 userland (xnu-12377.122.4). The wire protocol is what Apple's
 * stock Metal.framework + IOGPU.framework speak to IOGPUDeviceUserClient.
 *
 * VERSION NOTE [claim A17-kernel-version, verify-kernel-26.5.json]: in iOS
 * 26.5 (xnu-12377.122.4) BOTH com.apple.iokit.IOGPUFamily and the IOGPU
 * framework image are version 130.14 (LC_SOURCE_VERSION 130.14.0.0.0). The
 * earlier 26.6/130.16 numbers in spec 01 are superseded here; the selector
 * SHAPES are identical across these builds (re-verified by A14-selectors and
 * A1-dispatch-tables against the 26.5 DSC/kernelcache), so this header is the
 * single contract for the 26.5 target.
 *
 * Every selector number, offset, and struct below carries a citation to the
 * verified ledger (99-claims-ledger.json / verify-kernel-26.5.json /
 * verify-dsc-26.5.json) or to spec file:line (01-iogpu-userclient-abi.md,
 * 02-iogpu-caller-flow.md). 26.5 values are preferred wherever they differ.
 *
 * Our kernel implements ONLY dispatch table A ("unrestricted"): never set the
 * restricted flag [spec 01 section 2.2, claim A13-uc-dispatch]. Table B (which
 * NAKs the privileged selectors) is not provided.
 */
#ifndef IOGPU_UC_ABI_H
#define IOGPU_UC_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * 0. Dispatch geometry and discovery
 * ===================================================================== */

/*
 * 56 external-method selectors (0..55) and 14 external traps (0..13).
 * [claim A1-sel-table, A4-trap-table / A2-trap-table]
 *   externalMethod rejects selector > 0x37 to the base IOUserClient path
 *   (the vendor extension window; AGX uses 0x107/0x108).
 *   getTargetAndTrapForIndex rejects trap index > 13.
 */
#define IOGPU_UC_NUM_SELECTORS      56u
#define IOGPU_UC_NUM_TRAPS          14u
#define IOGPU_UC_MAX_SELECTOR       0x37u   /* cmp w1,#0x37 in externalMethod */

/*
 * Service discovery contract [verify-dsc-26.5.json A1-match-class, A5-discovery;
 * verify-dsc A18-plugin-discovery (the A18 CORRECTION)].
 *
 *  - The accelerator service must derive from a class named "IOAcceleratorES"
 *    (Metal matches by IOServiceMatching("IOAcceleratorES")). Our IOGPU base
 *    publishes it; see mali_backend_ops.h for the accelerator object.
 *  - Registry properties read on the publish notification:
 *       MetalPluginName       (CFString, required) -> bundle at
 *                             /System/Library/Extensions/<name>.bundle
 *       MetalPluginClassName  (CFString, optional) -> [bundle classNamed:],
 *                             else [bundle principalClass]
 *  - A18 CORRECTION: Metal's own discovery path
 *    (-[MTLIOAccelServiceGlobalContext init] / _getMetalPluginClassForService)
 *    verifies the plug-in class with isSubclassOfClass:_MTLDevice, NOT
 *    IOGPUMetalDevice. The IOGPUMetalDevice requirement lives only in the
 *    uncalled +[IOGPUMetalDevice registerService:]. THEREFORE our Metal
 *    plug-in device class MUST subclass _MTLDevice. Subclassing
 *    IOGPUMetalDevice satisfies that (IOGPUMetalDevice : _MTLDevice) and reuses
 *    its selector-calling machinery, so that is the recommended base.
 */
#define IOGPU_MATCH_CLASS           "IOAcceleratorES"
#define IOGPU_PROP_PLUGIN_NAME      "MetalPluginName"
#define IOGPU_PROP_PLUGIN_CLASS     "MetalPluginClassName"

/*
 * IOServiceOpen connect type [verify A3-uc-type; spec 02 section 3 step 2].
 *   type = IOGPU_UC_CONNECT_TYPE | (options << 16), options < 0x10000.
 *   Kernel newUserClient accepts (type & 0xffff) == 1 (device user client) and
 *   == 0x21 (memory-info user client). options = type >> 16.
 */
#define IOGPU_UC_CONNECT_TYPE_DEVICE    1u
#define IOGPU_UC_CONNECT_TYPE_MEMINFO   0x21u
#define IOGPU_UC_CONNECT_OPTIONS_MAX    0x10000u

/* ===================================================================== *
 * 1. IOReturn values this ABI uses [spec 01 Appendix B]
 * ===================================================================== */
#define kIOGPUReturnSuccess         0x00000000u
#define kIOGPUReturnError           0xe00002bcu
#define kIOGPUReturnNoMemory        0xe00002bdu
#define kIOGPUReturnNotPrivileged   0xe00002c1u
#define kIOGPUReturnBadArgument     0xe00002c2u   /* unknown id, size mismatch, async ref */
#define kIOGPUReturnUnsupported     0xe00002c7u   /* stub return for optional selectors */
#define kIOGPUReturnInternalError   0xe00002c9u
#define kIOGPUReturnNotAttached     0xe00002d8u   /* uc not started (uc[0x100]&1 clear) */
#define kIOGPUReturnNotPermitted    0xe00002e2u
/*
 * 0xe0078001 is the queue-limit code: IOGPU.framework calls abort() on it.
 * OUR KERNEL MUST NEVER RETURN IT [spec 01 section 2.3, claim A8-queue-create].
 */
#define kIOGPUReturnQueueLimit_DO_NOT_RETURN 0xe0078001u

/* ===================================================================== *
 * 2. External-method selectors (dispatch table A)
 * -----------------------------------------------------------------------
 * The sIn/bIn/sOut/bOut counts are the IOExternalMethodDispatch columns
 * (V = variable = kIOUCVariableStructureSize = 0xffffffff).
 * [claim A14-selectors, A1-sel-table; spec 01 section 3]
 * Req legend for the minimal CoreAnimation driver [spec 01 section 8]:
 *   M must, S should, O optional (stub with kIOGPUReturnUnsupported), - unused.
 * ===================================================================== */
enum iogpu_uc_selector {
    kIOGPUSel_GetDeviceInfo             = 0,   /* bOut 0x40                M */
    kIOGPUSel_GetName                   = 1,   /* bOut 0x40                S */
    kIOGPUSel_GetStampConfig            = 2,   /* bOut 0x218               M */
    kIOGPUSel_GetTraceFilter            = 3,   /* bOut 8                   O */
    kIOGPUSel_GetClientInfo             = 4,   /* bOut 0x20                M */
    kIOGPUSel_GetNextGlobalTraceID      = 5,   /* bOut 0x10                S */
    kIOGPUSel_NewCommandQueue           = 6,   /* bIn V, bOut 0x10         M */
    kIOGPUSel_DeleteCommandQueue        = 7,   /* sIn 1                    M */
    kIOGPUSel_NewResource               = 8,   /* bIn V, bOut V            M */
    kIOGPUSel_ReleaseResource           = 9,   /* sIn 1 (twin of trap 1)   - */
    kIOGPUSel_ResourceFinishEvent       = 10,  /* sIn 2 (twin of trap 2)   - */
    kIOGPUSel_SetResourcePurgeable      = 11,  /* sIn 2, sOut 1 (twin t3)  - */
    kIOGPUSel_CreateDeviceShmem         = 12,  /* sIn 2, bOut 0x10         M */
    kIOGPUSel_DestroyDeviceShmem        = 13,  /* sIn 1                    M */
    kIOGPUSel_CreateNotificationQueue   = 14,  /* sIn 2, bOut 0x10         M */
    kIOGPUSel_DestroyNotificationQueue  = 15,  /* sIn 1                    M */
    kIOGPUSel_GetDeviceHandle           = 16,  /* bOut 8                   - */
    kIOGPUSel_CreateFence               = 17,  /* bOut 4                   O */
    kIOGPUSel_DestroyFence              = 18,  /* sIn 1                    O */
    kIOGPUSel_CreateMTLEvent            = 19,  /* sIn 1, bOut 0x18         O */
    kIOGPUSel_DestroyMTLEvent           = 20,  /* sIn 1                    O */
    kIOGPUSel_GetMemoryData             = 21,  /* bOut 0x30                O */
    kIOGPUSel_Reserved22                = 22,  /* stub -> Unsupported      - */
    kIOGPUSel_GetAllocatedSize          = 23,  /* sOut 1                   S */
    kIOGPUSel_SetQueueNotificationQueue = 24,  /* sIn 2                    M */
    kIOGPUSel_SubmitCommandBuffers      = 25,  /* sIn 4, bIn V, sOut 1     M */
    kIOGPUSel_SetQueuePriority          = 26,  /* sIn 1, bIn 12, sOut 1    O */
    kIOGPUSel_SetQueueQoS               = 27,  /* sIn 1, bIn 4             O */
    kIOGPUSel_CreateLateEvalEvent       = 28,  /* sOut 2                   O */
    kIOGPUSel_DestroyLateEvalEvent      = 29,  /* sIn 1                    O */
    kIOGPUSel_SignalLateEvalEvent       = 30,  /* sIn 2                    O */
    kIOGPUSel_QueryLateEvalEvent        = 31,  /* sIn 1, sOut 2            O */
    kIOGPUSel_SetDisplayParamsHook      = 32,  /* sIn 2 (twin of trap 8)   - */
    kIOGPUSel_SetAppGPURole             = 33,  /* sIn 2                    O */
    kIOGPUSel_GetAppGPURole             = 34,  /* sIn 1, sOut 1            O */
    kIOGPUSel_SetResourceOwnerIdentity  = 35,  /* sIn 2                    O */
    kIOGPUSel_ResourceCreateIOSurface   = 36,  /* sIn 3, sOut 1 (ANE)      O */
    kIOGPUSel_ResourceDetachBacking     = 37,  /* sIn 1                    O */
    kIOGPUSel_ResourceReplaceBytes      = 38,  /* bIn 0x18                 O */
    kIOGPUSel_ResourceReplaceRanges     = 39,  /* bIn 0x18, sOut 1         O */
    kIOGPUSel_CreateVNIODesc            = 40,  /* sIn 1, sOut 2            O */
    kIOGPUSel_DestroyVNIODesc           = 41,  /* sIn 1                    O */
    kIOGPUSel_CreateIOCommandQueue      = 42,  /* sIn 2, sOut 2            O */
    kIOGPUSel_DestroyIOCommandQueue     = 43,  /* sIn 1                    O */
    kIOGPUSel_SetIONotificationQueue    = 44,  /* sIn 2                    O */
    kIOGPUSel_SubmitIOCommands          = 45,  /* sIn 1, bIn V             O */
    kIOGPUSel_CreateIOCommandBuffer     = 46,  /* sIn 1, sOut 2            O */
    kIOGPUSel_DestroyIOCommandBuffer    = 47,  /* sIn 2                    O */
    kIOGPUSel_TryCancelIOCommandBuffer  = 48,  /* sIn 2                    O */
    kIOGPUSel_PerformIO                 = 49,  /* sIn 1                    O */
    kIOGPUSel_IOCommandBufferComplete   = 50,  /* sIn 1                    O */
    kIOGPUSel_IOCommandBufferBarrier    = 51,  /* sIn 3                    O */
    kIOGPUSel_GroupAddResources         = 52,  /* sIn 2, bIn V             S */
    kIOGPUSel_GroupRemoveResources      = 53,  /* sIn 2, bIn V             S */
    kIOGPUSel_CreateDeviceAssertion     = 54,  /* sIn 2, sOut 1            O */
    kIOGPUSel_DeviceNoArgHook           = 55,  /* (no args)                - */
};

/* ===================================================================== *
 * 3. External traps (table A, 14 entries) [spec 01 section 4; A2-trap-table]
 *   IOConnectTrapN(conn, idx, p1..pN); kernel fn receives (this, p1..p6).
 * ===================================================================== */
enum iogpu_uc_trap {
    /* submit ONE command buffer: p1=queueId, p2=argSize(<=0x40),
     * p3=user ptr to args, p4=user ptr to u32 status out.
     * Used by the framework when count==1 && cmdBufArgSize<=0x40. */
    kIOGPUTrap_SubmitOne                = 0,
    kIOGPUTrap_ReleaseResource          = 1,   /* p1=resourceId (== sel 9)     */
    kIOGPUTrap_ResourceFinishEvent      = 2,   /* p1=resId, p2=slot(0/1)       */
    kIOGPUTrap_SetResourcePurgeable     = 3,   /* p1=resId, p2=state(1..6), p3=u64 out */
    kIOGPUTrap_GroupAdd                 = 4,   /* p1=groupId, p2=count(<=0x100), p3=ptr/val */
    kIOGPUTrap_GroupRemove              = 5,   /* p1=groupId, p2=count, p3=u64[] ptr   */
    kIOGPUTrap_GroupUpdate              = 6,   /* remove-then-add combined             */
    kIOGPUTrap_QueueSetResourceGroups   = 7,   /* p1=queueId, p2=count(<=0x28), p3=u32[] */
    kIOGPUTrap_SetDisplayParams         = 8,   /* p1,p2 -> device hook (default Unsupported) */
    kIOGPUTrap_QueueSignalSharedEvent   = 9,   /* p1=queueId, p2=port, p3/p4=val lo/hi */
    kIOGPUTrap_QueueWaitSharedEvent     = 10,  /* + p5=timeout                         */
    kIOGPUTrap_QueueSignalMTLEvent      = 11,  /* p1=queueId, p2=eventId, p3/p4=val    */
    kIOGPUTrap_QueueWaitMTLEvent        = 12,  /* + p5=timeout                         */
    kIOGPUTrap_QueueSupportsBgRole      = 13,  /* p1=queueId (0=device default)        */
};
#define IOGPU_TRAP0_MAX_ARG_SIZE    0x40u   /* trap 0 copies <= 0x40 bytes [A4-trap-table] */

/* ===================================================================== *
 * 4. IOExternalMethodArguments offsets the handlers read [spec 01 section 2.3].
 *   Provided so the kernel dispatcher and the plug-in share constants.
 * ===================================================================== */
#define IOGPU_EMA_SCALAR_INPUT          0x20u
#define IOGPU_EMA_SCALAR_INPUT_COUNT    0x28u
#define IOGPU_EMA_STRUCT_INPUT          0x30u
#define IOGPU_EMA_STRUCT_INPUT_SIZE     0x38u
#define IOGPU_EMA_STRUCT_INPUT_DESC     0x40u   /* != NULL when > inband limit */
#define IOGPU_EMA_SCALAR_OUTPUT         0x48u
#define IOGPU_EMA_SCALAR_OUTPUT_COUNT   0x50u
#define IOGPU_EMA_STRUCT_OUTPUT         0x58u
#define IOGPU_EMA_STRUCT_OUTPUT_SIZE    0x60u

/* ===================================================================== *
 * 5. Selector wire structs. All little-endian. [spec 01 section 5]
 * ===================================================================== */

/* -- sel 0 GET_DEVICE_INFO (bOut 64) [A1-sel-table; spec 01 5.1] -------- */
typedef struct iogpu_device_info {       /* 0x40 bytes */
    uint32_t configBits;                 /* +0x00 bit6 = supportPriorityBand */
    uint32_t deviceBits;                 /* +0x04 */
    uint64_t textureRam;                 /* +0x08 */
    uint64_t videoRam;                   /* +0x10 0 = unified memory */
    uint32_t accelID;                    /* +0x18 accelerator id (IOSurfaceBindAccel) */
    uint32_t _pad1c;                     /* +0x1c */
    uint64_t sharedMemorySize;           /* +0x20 */
    uint64_t maxResourceSize;            /* +0x28 maxBufferLength */
    uint64_t reserved0;                  /* +0x30 */
    uint32_t reserved1;                  /* +0x38 */
    uint32_t reserved2;                  /* +0x3c */
} iogpu_device_info_t;

/* Selector numbers used by the in-kernel self-test and callers (spec 01 dispatch table). */
#define IOGPU_SEL_GET_STAMP_CONFIG    2u
#define IOGPU_SEL_NEW_COMMAND_QUEUE   6u
#define IOGPU_SEL_NEW_RESOURCE        8u
#define IOGPU_SEL_CREATE_DEVICE_SHMEM 12u

/* -- sel 2 GET_STAMP_CONFIG (bOut 0x218 = 536) [A7-sel2-layout / A3-sel2-layout] */
#define IOGPU_STAMP_CONFIG_SIZE     0x218u
#define IOGPU_STAMP_MAX_SLOTS       128u   /* (0x218 - 0x18) / 4 */
typedef struct iogpu_stamp_config {      /* client zero-fills before the call */
    uint64_t stampBase;                  /* +0x00 user VA of mapped stamp memory + uc[0x118] */
    uint32_t cfg0;                       /* +0x08 accel config word */
    uint32_t cfg1;                       /* +0x0c */
    uint32_t privateSize;                /* +0x10 per-resource private tail size (sel 8 reply) */
    uint32_t nStampSlots;                /* +0x14 count of event-machine channels */
    uint32_t stampOff[IOGPU_STAMP_MAX_SLOTS]; /* +0x18 per-channel u32 offsets from stampBase */
} iogpu_stamp_config_t;
/*
 * Minimal driver: nStampSlots = 1, stampOff[0] = 0 over a zeroed kernel-written
 * read-only page; cfg0 = 0. Stamp counters hold values pre-shifted left by 8
 * (low byte zero) [spec 01 5.8]; the event-complete test is
 *   (int32)((target&~0xff) - *stampSlot) <= 0.
 */

/* -- sel 4 GET_CLIENT_INFO (bOut 0x20) [A6-device-open-order; spec 01 5.1] - */
typedef struct iogpu_client_info {       /* 0x20 bytes */
    uint64_t clientId;                   /* +0x00 process-unique */
    uint64_t sharedAddrA;                /* +0x08 optional block (stored iff +0x18 >= 4) */
    uint64_t deviceShared;               /* +0x10 device client-shared VA (0 -> framework falls back to sel 23) */
    uint32_t sharedALen;                 /* +0x18 length of block A */
    uint32_t _pad1c;                     /* +0x1c */
} iogpu_client_info_t;

/* -- sel 12 CREATE_DEVICE_SHMEM (sIn 2, bOut 0x10) [A18-K-shmem / A5-shmem] -- */
enum iogpu_shmem_type {                  /* scalar in[1]; pool types */
    kIOGPUShmemSegmentList   = 0,        /* initial 0x4000 */
    kIOGPUShmemKernelCommand = 1,        /* initial 0x4000 */
    kIOGPUShmemDebugBuffer   = 2,        /* one VM page */
    kIOGPUShmemSideband      = 3,        /* initial 0x4000, grows x2 to 2 MiB then +1 MiB */
};
#define IOGPU_SHMEM_SIZE_MAX        0x10000000u  /* scalar in[0] must be < this + 1 */
typedef struct iogpu_device_shmem_reply { /* bOut 0x10; all three must be non-zero */
    uint64_t userAddress;                /* +0x00 client VA */
    uint32_t size;                       /* +0x08 actual size */
    uint32_t id;                         /* +0x0c shmem id in this device's namespace */
} iogpu_device_shmem_reply_t;

/* -- sel 14 CREATE_NOTIFICATION_QUEUE (sIn 2, bOut 0x10) [A6-notif-queue] --- */
#define IOGPU_NQ_MAX_ENTRIES        0x2000u  /* scalar in[0] 1..0x2000 */
#define IOGPU_NQ_MAX_ENTRY_SIZE     0x28u    /* scalar in[1] 1..0x28 */
#define IOGPU_NQ_CMDQUEUE_ENTRIES   0x100u   /* command queues use (0x100, 0x28) */
#define IOGPU_NQ_CMDQUEUE_ESIZE     0x28u
#define IOGPU_NQ_IOQUEUE_ENTRIES    0x2000u  /* IO queues use (0x2000, 0x10) */
#define IOGPU_NQ_IOQUEUE_ESIZE      0x10u
typedef struct iogpu_notification_queue_reply { /* bOut 0x10 */
    uint64_t queueAddress;               /* +0x00 client VA (IODataQueueMemory) */
    uint32_t id;                         /* +0x08 non-zero namespace id */
    uint32_t _pad0c;
} iogpu_notification_queue_reply_t;
/*
 * After sel 14 the framework calls IODataQueueAllocateNotificationPort() and
 * IOConnectSetNotificationPort(conn, type=0, port, refCon=queueId). The kernel
 * registerNotificationPort override requires type==0 and binds the port to the
 * queue via the IODataQueue "data available" message [spec 01 5.3].
 */
#define IOGPU_NOTIFICATION_PORT_TYPE  0u

/*
 * Completion entry written by the kernel into the command-queue notification
 * ring, EXACTLY 0x28 bytes [claim A11-notif-entry; spec 01 5.3].
 *   ref: echoed from args.scheduledRef (+0x10) or completedRef (+0x18).
 *   t0/t1: mach_absolute_time ticks; the client converts with the mach timebase.
 *   status: 0 = success, else an IOGPU command-buffer status (table in 7.).
 *   extra: GPU fault address for completions, 0 for "scheduled".
 */
typedef struct iogpu_completion_entry { /* 0x28 bytes */
    uint64_t ref;                        /* +0x00 */
    uint64_t tBegin;                     /* +0x08 */
    uint64_t tEnd;                       /* +0x10 */
    uint32_t status;                     /* +0x18 */
    uint32_t _pad1c;                     /* +0x1c (status copyout is 4 bytes) */
    uint64_t extra;                      /* +0x20 */
} iogpu_completion_entry_t;

/* IO-queue entry, 0x10 bytes [spec 01 9.1]: block invoked as block(block,status). */
typedef struct iogpu_io_completion_entry {
    uint64_t block;                      /* +0x00 */
    uint32_t status;                     /* +0x08 */
    uint32_t _pad0c;
} iogpu_io_completion_entry_t;

/* -- sel 6 NEW_COMMAND_QUEUE (bIn V, bOut 0x10) [A10-queue-create / A8-queue-create] */
#define IOGPU_NEW_QUEUE_ARGS_MIN    0x408u  /* struct in must be >= 0x408 and == accel queue-args size */
typedef struct iogpu_new_command_queue_args { /* generic prefix 0x408; vendor tail follows */
    char     processPath[0x400];         /* +0x000 proc_pidpath */
    uint32_t qosLevel;                   /* +0x400 */
    uint8_t  openGLQueue;                /* +0x404 */
    uint8_t  backgroundFlag;             /* +0x405 */
    uint8_t  _pad406[2];                 /* +0x406 */
    /* vendor tail: the Mali plug-in may append fields; total MUST equal the
     * accelerator's configured queue-args size (populateAccelConfig +0x90). */
} iogpu_new_command_queue_args_t;
typedef struct iogpu_new_command_queue_reply { /* bOut 0x10 */
    uint64_t queueId;                    /* +0x00 non-zero (0 == failure) */
    uint64_t traceObjectId;              /* +0x08 */
} iogpu_new_command_queue_reply_t;

/* -- sel 8 NEW_RESOURCE (bIn V >= 0x58, bOut V) [A15-resource-create / A9-resource-args] */
#define IOGPU_RESOURCE_ARGS_MIN     0x58u
enum iogpu_resource_type {               /* args +0x00; kernel accepts {0,3,0x40,0x80,0x82} */
    kIOGPUResType_KernelMem  = 0x00,     /* kernel-allocated system memory */
    kIOGPUResType_Group      = 0x03,     /* resource group (args +0x38 = capacity) */
    kIOGPUResType_Sparse     = 0x40,     /* virtual/sparse placement */
    kIOGPUResType_ClientMem  = 0x80,     /* client-supplied memory / address ranges */
    kIOGPUResType_IOSurface  = 0x82,     /* IOSurface plane (id +0x38, plane +0x3c, field +0x40) */
};
enum iogpu_resource_flag {               /* args +0x14 */
    kIOGPUResFlag_ZeroFill    = 0x40,      /* bit6: kernel zero-fills the CPU mapping */
    kIOGPUResFlag_Suballoc    = 0x800,     /* bit11: args +0x50 = parent resource id */
    kIOGPUResFlag_WantCPUMap  = 0x2000,    /* bit13: request a CPU mapping */
    kIOGPUResFlag_AddrRanges  = 0x200000,  /* bit21: client address-ranges variant (type 0x80) */
};
typedef struct iogpu_new_resource_args { /* generic 0x58; vendor tail follows */
    uint32_t resType;                    /* +0x00 enum iogpu_resource_type */
    uint32_t cacheMode;                  /* +0x04 bits 8..15 = cache mode 0..9 (types 0/3/0x40) */
    uint32_t field08;                    /* +0x08 framework constant 0x10001 */
    uint16_t field0c;                    /* +0x0c framework constant 1 */
    uint16_t _pad0e;
    uint8_t  flags10[4];                 /* +0x10 framework constant {01,01,00,01} */
    uint32_t flags;                      /* +0x14 enum iogpu_resource_flag */
    uint32_t _pad18hi;                   /* +0x18 (u64 arg18, 0 from framework) */
    uint32_t _pad1c;
    uint64_t gpuAddress;                 /* +0x20 requested fixed GPU VA (0 = kernel chooses) */
    uint64_t gpuLength;                  /* +0x28 length of the fixed mapping */
    uint32_t units;                      /* +0x30 max(1, this) -> accel resource init */
    uint32_t gpuTagHi;                   /* +0x34 OR-ed into bits 63..32 of returned GPU VA */
    uint64_t ptrOrSurface;               /* +0x38 client VA / IOAddressRange* / {surfaceId,plane} */
    uint64_t ptrOrField;                 /* +0x40 second ptr / range count / {field,0} */
    uint64_t size;                       /* +0x48 resource size in bytes */
    uint32_t parentIdOrSparseTile;       /* +0x50 parent id (suballoc) or sparse tile size */
    uint32_t _pad54;
} iogpu_new_resource_args_t;
/* reply size passed by the framework = 0x58 + privateSize (sel 2 +0x10). */
typedef struct iogpu_new_resource_reply { /* generic 0x58; private tail follows [A16-resource-out / A10-resource-reply] */
    uint64_t gpuAddress;                 /* +0x00 GPU VA (| gpuTagHi<<32) */
    uint64_t cpuAddress;                 /* +0x08 client CPU VA (0 if none) */
    uint64_t clientShared;               /* +0x10 client-shared block VA (two 0x40 IOGPUEvent slots) */
    uint64_t metadataPtr;                /* +0x18 (type 0x40 only) */
    uint32_t replyFlags;                 /* +0x20 bit0 = client must zero-fill cpuAddress */
    uint32_t resourceId;                 /* +0x24 non-zero handle (p1 of traps 1/2/3; list ids) */
    uint64_t residentSize;               /* +0x28 */
    uint64_t traceObjectId;              /* +0x30 */
    uint64_t allocId0;                   /* +0x38 allocation identifier (group remove key) */
    uint64_t allocId1;                   /* +0x40 */
    uint64_t protection;                 /* +0x48 */
    uint64_t gpuVALength;                /* +0x50 */
    /* uint8_t privateTail[privateSize]; +0x58 copied to the plug-in's resource */
} iogpu_new_resource_reply_t;

/* ===================================================================== *
 * 6. Submission framing [claim A12-submit / A11-cmdbuf-args / A13-cmdbuf-args]
 * ===================================================================== */
/*
 * sel 25 SUBMIT_COMMAND_BUFFERS: scalar in {queueId, flags(0), count, argSize}.
 *   argSize MUST equal the accelerator's configured cmdBufArgs size
 *   (populateAccelConfig +0x68 == accel+0x268), and struct-in size must equal
 *   count*argSize. scalar out[0] = u32 queue status.
 * The generic IOGPUMetalDevice returns cmdBufArgsSize == 0x30, so our plug-in
 * and kernel simply agree on 0x30 [claim A13-cmdbuf-args; spec 02 section 8].
 */
#define IOGPU_CMDBUF_ARGS_SIZE      0x30u  /* -[IOGPUMetalDevice cmdBufArgsSize] */
#define IOGPU_CMDBUF_ARGS_MIN       0x28u  /* vendor tail may extend past 0x28 */

/* sIOGPUCommandQueueCommandBufferArgs generic prefix (0x30) [spec 01 5.6] */
typedef struct iogpu_cmdbuf_args {
    uint32_t kernelCommandShmemId;       /* +0x00 type-1 shmem id (K) */
    uint32_t segmentListShmemId;         /* +0x04 type-0 shmem id (S) */
    uint32_t sidebandShmemId;            /* +0x08 type-3 shmem id (0 if none) */
    uint32_t reserved0c;                 /* +0x0c */
    uint64_t scheduledRef;               /* +0x10 echoed in "scheduled" notification (0 = none) */
    uint64_t completedRef;               /* +0x18 echoed in "completed" notification */
    uint32_t debugBufferShmemId;         /* +0x20 type-2 shmem id (0 if none) */
    uint32_t reserved24;                 /* +0x24 */
    uint32_t reserved28;                 /* +0x28 (Metal4 writes globalTraceObjectID here) */
    uint32_t reserved2c;                 /* +0x2c */
    /* vendor tail from +0x30 if IOGPU_CMDBUF_ARGS_SIZE is raised */
} iogpu_cmdbuf_args_t;

/* ===================================================================== *
 * 6a. Kernel command buffer (shmem K) - 4-byte-aligned records [spec 01 5.7]
 *   { u32 type; u32 size (incl. 8-byte header, >=8, %4==0); payload }.
 *   The generic kernel enforces size>=8, size%4==0, cmd+size<=end; a bad
 *   record sets queue status 0xa. [claim A18-K-shmem]
 * ===================================================================== */
typedef struct iogpu_kcmd_header {
    uint32_t type;                       /* enum iogpu_kcmd_type */
    uint32_t size;
} iogpu_kcmd_header_t;

enum iogpu_kcmd_type {                   /* generic types understood by the family */
    kIOGPUKCmd_DebugLog         = 0,
    kIOGPUKCmd_SecondFamily     = 1,
    kIOGPUKCmd_SleepMS          = 2,     /* {u32 ms} size 0x10 */
    kIOGPUKCmd_SignalSharedEvent= 3,     /* {u32 port; u32 0; u64 value} 0x18 */
    kIOGPUKCmd_WaitSharedEvent  = 4,     /* {u32 port; u32 timeout; u64 value} */
    kIOGPUKCmd_SignalMTLEvent   = 5,     /* {u32 eventId; u32 0; u64 value} */
    kIOGPUKCmd_WaitMTLEventBar  = 6,     /* wait with barrier */
    kIOGPUKCmd_PurgeResources   = 8,     /* {u32 count; u32 ids[count]} */
    kIOGPUKCmd_SetProtection    = 9,     /* {u64 options} 0x10 */
    kIOGPUKCmd_WaitMTLEvent     = 10,    /* wait without barrier */
    kIOGPUKCmd_SignalSEventAgent= 11,    /* {port;0;u64 value;u64 agentMask} 0x20 */
    kIOGPUKCmd_CondEventAbort   = 12,
    kIOGPUKCmd_SignalSEventSched= 13,
    kIOGPUKCmd_ResponsibleTasks = 14,    /* {u32 count; u32 ids[count]} */
    /*
     * type 15 SetResourceGroups is the FIRST record of every command buffer,
     * reserved by the framework as {type=0xf, size=0xac}; 0xa4-byte body of
     * {u32 count; u32 groupIds[40]}; count 0 when unused [claim A12-kernel-commands].
     */
    kIOGPUKCmd_SetResourceGroups= 15,
    kIOGPUKCmd_UpdateMappings   = 16,    /* Metal 4 */
    kIOGPUKCmd_CopyMappings     = 17,    /* Metal 4 */
    kIOGPUKCmd_PostMappingWait  = 18,    /* Metal 4 */
    /*
     * VENDOR EXTENSION SPACE: types >= 0x13 are free for the plug-in/kernel
     * private agreement. THIS IS WHERE THE MALI JOB/CSG SUBMISSION DESCRIPTION
     * LIVES. Our MaliCommandQueue::processSegmentKernelCommand decodes these
     * (see mali_backend_ops.h MALI_KCMD_*). The generic kernel would set queue
     * status 0xa for any unknown type, so the Mali queue subclass must consume
     * them before delegating [spec 01 5.7, spec 03 5.6, spec 06 section 3].
     */
    kIOGPUKCmd_VendorBase       = 0x13,
};
#define IOGPU_KCMD_SETGROUPS_SIZE   0xacu  /* reserved first record total bytes */
#define IOGPU_KCMD_SETGROUPS_HDR    0x000000ac0000000fULL /* {type=0xf,size=0xac} literal */
#define IOGPU_KCMD_MAX_GROUPS       40u

/* ===================================================================== *
 * 6b. Segment-list shmem (shmem S) - a chain of 0x10-byte-header blocks.
 *   [claim A19-S-layout; spec 01 5.7]
 * ===================================================================== */
typedef struct iogpu_seg_block_header { /* 0x10 */
    uint64_t tag;                        /* +0x00 opaque client token */
    uint32_t count;                      /* +0x08 segments (type0) / {start,end} pairs (type1) */
    uint32_t control;                    /* +0x0c bit31=last block; bit30=typed; low bits=size/type */
} iogpu_seg_block_header_t;
#define IOGPU_SEG_CTRL_LAST         (1u << 31)
#define IOGPU_SEG_CTRL_TYPED        (1u << 30)
#define IOGPU_SEG_KCLIST_CONTROL    0x40000001u /* type-1 KC-list block control word */

typedef struct iogpu_segment_header {   /* 0x20, type-0 block, per segment */
    uint64_t tag;                        /* +0x00 */
    uint32_t kcStart;                    /* +0x08 offset in K buffer */
    uint32_t kcEnd;                      /* +0x0c kcStart <= kcEnd <= K length */
    uint32_t sidebandOff;                /* +0x10 offset in sideband shmem */
    uint32_t _pad14;                     /* +0x14 */
    uint32_t nResources;                 /* +0x18 distinct resources */
    uint32_t nGroups;                    /* +0x1c count of 0x40-byte resource entries (< 2^26) */
    /* iogpu_resource_group_entry entries[nGroups]; +0x20 */
} iogpu_segment_header_t;

typedef struct iogpu_resource_group_entry { /* 0x40, up to 6 resources */
    uint32_t resId[6];                   /* +0x00 */
    uint32_t sizeKiB[6];                 /* +0x18 ceil(residentSize/1024) */
    uint16_t usage[6];                   /* +0x30 Read=1 Write=2 (legacy Sample=4) */
    uint16_t _pad3c;                     /* +0x3c */
    uint16_t n;                          /* +0x3e valid id count (< 7) */
} iogpu_resource_group_entry_t;

/* ===================================================================== *
 * 7. Completion status -> MTLCommandBufferError mapping [spec 01 5.9]
 *   19-entry table; index = IOGPU status, value = MTL error; status>0x12 -> 1.
 *   Status the kernel submit path itself produces:
 *     8 shmem not mappable into kernel; 9 invalid/missing segment shmem;
 *     0xa parse/validation failure; 4 queue denied. Success MUST be 0.
 * ===================================================================== */
static const uint8_t iogpu_status_to_mtl_error[19] = {
    0, 1, 2, 2, 4, 1, 7, 7, 8, 9, 1, 3, 10, 10, 1, 1, 16, 12, 17
};
#define kIOGPUStatus_Success        0u
#define kIOGPUStatus_QueueDenied    4u
#define kIOGPUStatus_ShmemUnmapped  8u
#define kIOGPUStatus_SegmentBad     9u
#define kIOGPUStatus_ParseError     0xau

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* IOGPU_UC_ABI_H */
