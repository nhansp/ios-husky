/*
 * iogpu_smoke.c - IOGPU user-client self-test from iOS userland.
 * -------------------------------------------------------------------------
 * Target: iOS 26.5 arm64 userland executable running on Pixel 8 Pro (husky, Mali-G715).
 * Ports the in-kernel self-test (refs/husky_iogpu_selftest.cpp) to userland via
 * Mach/IOKit user-client interfaces:
 *   - IOServiceGetMatchingService / IOServiceMatching("IOAcceleratorES")
 *   - IOServiceOpen(service, mach_task_self(), IOGPU_UC_CONNECT_TYPE_DEVICE, &connect)
 *   - Step 2: IOConnectCallStructMethod (Selector 2: GET_STAMP_CONFIG)
 *   - Shmems: IOConnectCallMethod (Selector 12: CREATE_DEVICE_SHMEM)
 *   - Step 3: IOConnectCallStructMethod (Selector 8: NEW_RESOURCE + 'MBO1' tail)
 *   - Step 4: IOConnectCallStructMethod (Selector 6: NEW_COMMAND_QUEUE)
 *   - Step 5: IOConnectTrap4 (Trap 0: SUBMIT_ONE)
 *   - Step 6: Verify sync completion (synchronous kernel poll in submitCommandBuffers)
 *   - Step 7: Verify all 256 computed values through user CPU mapping
 *   - Teardown in reverse order (Sel 7 queue, Trap 1 resource, Sel 13, IOServiceClose)
 *
 * Citing ABI contracts:
 *   - refs/iogpu_uc_abi.h
 *   - refs/IOGPU.cpp
 *   - refs/IOGPUDeviceUserClient.cpp
 *   - refs/IOGPUCommandQueue.cpp
 *   - refs/IOGPUMemory.cpp
 *   - refs/mali_backend_ops.h
 *   - refs/husky_gpu_compute.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include "iogpu_uc_abi.h"
#include "iogpu_internal.h"
#include "husky_gpu_compute.h"

/*
 * The macOS SDK IOKitLib.h (used on the include path for the iOS build) marks IOConnectTrapN
 * API_UNAVAILABLE(ios), but iOS IOKit.framework exports them (IOGPU's submit fast path is
 * IOConnectTrap4 index 0). Bind our own prototypes to the exported symbols.
 */
extern kern_return_t smoke_trap4(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4) __asm__("_IOConnectTrap4");
extern kern_return_t smoke_trap1(io_connect_t connect, uint32_t index, uintptr_t p1)
    __asm__("_IOConnectTrap1");

/* Compute input pattern [refs/husky_iogpu_selftest.cpp:110-114] */
static inline uint32_t comp_in(unsigned i)
{
	return 0x1000u + i * 0x11u;
}

/* Logging to stdout and /dev/console (PACKAGE.md line 46) */
static int g_console_fd = -1;

static void smoke_log_init(void)
{
	g_console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
}

static void smoke_log_close(void)
{
	if (g_console_fd >= 0) {
		close(g_console_fd);
		g_console_fd = -1;
	}
}

static void smoke_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (len <= 0) {
		return;
	}

	/* Write to stdout */
	fputs(buf, stdout);
	fflush(stdout);

	/* Write to /dev/console (kernel log) */
	if (g_console_fd >= 0) {
		ssize_t written = 0;
		while (written < len) {
			ssize_t ret = write(g_console_fd, buf + written, (size_t)(len - written));
			if (ret <= 0) {
				break;
			}
			written += ret;
		}
	}
}

static void print_cf_string_property(io_service_t service, const char *prop_name)
{
	CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, prop_name, kCFStringEncodingUTF8);
	if (!key) {
		smoke_log("IOGPU-SMOKE: property %s: failed to create CFString key\n", prop_name);
		return;
	}

	CFTypeRef val = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	CFRelease(key);

	if (!val) {
		smoke_log("IOGPU-SMOKE: property %s = (null)\n", prop_name);
		return;
	}

	if (CFGetTypeID(val) == CFStringGetTypeID()) {
		char buf[256];
		if (CFStringGetCString((CFStringRef)val, buf, sizeof(buf), kCFStringEncodingUTF8)) {
			smoke_log("IOGPU-SMOKE: property %s = \"%s\"\n", prop_name, buf);
		} else {
			smoke_log("IOGPU-SMOKE: property %s = (could not get C string)\n", prop_name);
		}
	} else {
		smoke_log("IOGPU-SMOKE: property %s = (non-string CFTypeID %lu)\n", prop_name, (unsigned long)CFGetTypeID(val));
	}
	CFRelease(val);
}

int main(void)
{
	int failed_step = 0;
	io_service_t service = IO_OBJECT_NULL;
	io_connect_t connect = IO_OBJECT_NULL;
	uint32_t kShmemId = 0;
	uint32_t sShmemId = 0;
	uint8_t *kBytes = NULL;
	uint8_t *sBytes = NULL;
	uint32_t resId = 0;
	uint8_t *resCpu = NULL;
	uint32_t queueId = 0;
	kern_return_t kr = kIOReturnSuccess;

	smoke_log_init();

	smoke_log("IOGPU-SMOKE: ============================================================\n");
	smoke_log("IOGPU-SMOKE: starting userland self-test on Mali-G715 (IOAcceleratorES)\n");
	smoke_log("IOGPU-SMOKE: ============================================================\n");

	/* ------------------------------------------------------------------
	 * [1/7] Find service and open device user-client
	 * [refs/iogpu_uc_abi.h:75, 85; refs/IOGPU.cpp:53-61, 162-191]
	 * ------------------------------------------------------------------ */
	smoke_log("IOGPU-SMOKE: [1/7] finding IOAcceleratorES service...\n");
	CFMutableDictionaryRef matching = IOServiceMatching(IOGPU_MATCH_CLASS);
	if (!matching) {
		smoke_log("IOGPU-SMOKE: [1/7] open device user-client: FAIL (IOServiceMatching returned NULL)\n");
		failed_step = 1;
		goto cleanup;
	}

	/* IOServiceGetMatchingService consumes one reference to matching dictionary */
	service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
	if (service == IO_OBJECT_NULL) {
		smoke_log("IOGPU-SMOKE: [1/7] open device user-client: FAIL (IOServiceGetMatchingService found no service, ret 0x%x)\n",
		          kIOReturnError);
		failed_step = 1;
		goto cleanup;
	}

	/* Print properties per PACKAGE.md line 30 */
	print_cf_string_property(service, IOGPU_PROP_PLUGIN_NAME);  /* "MetalPluginName" [refs/iogpu_uc_abi.h:76; refs/IOGPU.cpp:53] */
	print_cf_string_property(service, IOGPU_PROP_PLUGIN_CLASS); /* "MetalPluginClassName" [refs/iogpu_uc_abi.h:77; refs/IOGPU.cpp:54] */
	print_cf_string_property(service, "IOGPUName");             /* "Mali-G715" [refs/IOGPU.cpp:55, 82] */

	/* Open connection type 1 (IOGPU_UC_CONNECT_TYPE_DEVICE) [refs/iogpu_uc_abi.h:85; refs/IOGPU.cpp:166-175] */
	kr = IOServiceOpen(service, mach_task_self(), IOGPU_UC_CONNECT_TYPE_DEVICE, &connect);
	IOObjectRelease(service);
	service = IO_OBJECT_NULL;

	if (kr != kIOReturnSuccess || connect == IO_OBJECT_NULL) {
		smoke_log("IOGPU-SMOKE: [1/7] open device user-client: FAIL (IOServiceOpen ret 0x%x)\n", kr);
		failed_step = 1;
		goto cleanup;
	}
	smoke_log("IOGPU-SMOKE: [1/7] open device user-client: PASS (connect 0x%x, ret 0x0)\n", connect);

	/* ------------------------------------------------------------------
	 * [2/7] Selector 2: GET_STAMP_CONFIG ("create device")
	 * [refs/iogpu_uc_abi.h:119, 234-249; refs/IOGPUDeviceUserClient.cpp:212-227, 912]
	 * ------------------------------------------------------------------ */
	iogpu_stamp_config_t stampCfg;
	memset(&stampCfg, 0, sizeof(stampCfg));
	size_t stampCfgSize = sizeof(stampCfg);

	kr = IOConnectCallStructMethod(connect, IOGPU_SEL_GET_STAMP_CONFIG,
	                               NULL, 0,
	                               &stampCfg, &stampCfgSize);
	if (kr != kIOReturnSuccess || stampCfgSize < sizeof(stampCfg) || stampCfg.stampBase == 0) {
		smoke_log("IOGPU-SMOKE: [2/7] selector 2 (GET_STAMP_CONFIG): FAIL (ret 0x%x, stampBase 0x%llx, size %zu)\n",
		          kr, (unsigned long long)stampCfg.stampBase, stampCfgSize);
		failed_step = 2;
		goto cleanup;
	}
	smoke_log("IOGPU-SMOKE: [2/7] selector 2 (GET_STAMP_CONFIG): PASS (stampBase 0x%llx, %u slots, ret 0x0)\n",
	          (unsigned long long)stampCfg.stampBase, stampCfg.nStampSlots);

	/* ------------------------------------------------------------------
	 * Allocate K-shmem and S-shmem via Selector 12 (CREATE_DEVICE_SHMEM)
	 * [refs/iogpu_uc_abi.h:129, 237, 267-278; refs/IOGPUDeviceUserClient.cpp:419-450, 922]
	 * ------------------------------------------------------------------ */
	uint64_t sInK[2] = { 4096, kIOGPUShmemKernelCommand /* 1 */ };
	iogpu_device_shmem_reply_t kReply;
	memset(&kReply, 0, sizeof(kReply));
	size_t kReplySize = sizeof(kReply);

	kr = IOConnectCallMethod(connect, IOGPU_SEL_CREATE_DEVICE_SHMEM,
	                         sInK, 2, NULL, 0,
	                         NULL, NULL, &kReply, &kReplySize);
	if (kr != kIOReturnSuccess || kReplySize < sizeof(kReply) || kReply.userAddress == 0 || kReply.id == 0) {
		smoke_log("IOGPU-SMOKE: CREATE_DEVICE_SHMEM (K-shmem): FAIL (ret 0x%x, userAddr 0x%llx, id %u)\n",
		          kr, (unsigned long long)kReply.userAddress, kReply.id);
		failed_step = 2;
		goto cleanup;
	}
	kShmemId = kReply.id;
	kBytes = (uint8_t *)(uintptr_t)kReply.userAddress;
	smoke_log("IOGPU-SMOKE: CREATE_DEVICE_SHMEM (K-shmem): PASS (id %u, userAddr 0x%llx, size %u)\n",
	          kShmemId, (unsigned long long)kReply.userAddress, kReply.size);

	uint64_t sInS[2] = { 4096, kIOGPUShmemSegmentList /* 0 */ };
	iogpu_device_shmem_reply_t sReply;
	memset(&sReply, 0, sizeof(sReply));
	size_t sReplySize = sizeof(sReply);

	kr = IOConnectCallMethod(connect, IOGPU_SEL_CREATE_DEVICE_SHMEM,
	                         sInS, 2, NULL, 0,
	                         NULL, NULL, &sReply, &sReplySize);
	if (kr != kIOReturnSuccess || sReplySize < sizeof(sReply) || sReply.userAddress == 0 || sReply.id == 0) {
		smoke_log("IOGPU-SMOKE: CREATE_DEVICE_SHMEM (S-shmem): FAIL (ret 0x%x, userAddr 0x%llx, id %u)\n",
		          kr, (unsigned long long)sReply.userAddress, sReply.id);
		failed_step = 2;
		goto cleanup;
	}
	sShmemId = sReply.id;
	sBytes = (uint8_t *)(uintptr_t)sReply.userAddress;
	smoke_log("IOGPU-SMOKE: CREATE_DEVICE_SHMEM (S-shmem): PASS (id %u, userAddr 0x%llx, size %u)\n",
	          sShmemId, (unsigned long long)sReply.userAddress, sReply.size);

	/* ------------------------------------------------------------------
	 * [3/7] Selector 8: NEW_RESOURCE with BO-flags tail (GPU_EXEC | NONCACHEABLE)
	 * [refs/iogpu_uc_abi.h:125, 236, 341-390; refs/iogpu_internal.h:71-83;
	 *  refs/IOGPUDeviceUserClient.cpp:318-366, 918; refs/IOGPUMemory.cpp:54-207]
	 * ------------------------------------------------------------------ */
	mali_new_resource_args_t resIn;
	memset(&resIn, 0, sizeof(resIn));
	resIn.generic.resType    = kIOGPUResType_KernelMem; /* 0 */
	resIn.generic.size       = 0x8000;                  /* 32 KiB */
	resIn.generic.flags      = kIOGPUResFlag_WantCPUMap;
	resIn.generic.cacheMode  = 0;                       /* non-cacheable */
	resIn.generic.gpuAddress = HUSKY_COMP_JOB_VA;       /* 0x06004000: fixed VA */
	resIn.tail.magic         = MALI_RES_TAIL_MAGIC;     /* 'MBO1' = 0x4d424f31 */
	resIn.tail.bo_flags      = MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_GPU_EXEC |
	                           MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE;

	iogpu_new_resource_reply_t resOut;
	memset(&resOut, 0, sizeof(resOut));
	size_t resOutSize = sizeof(resOut);

	kr = IOConnectCallStructMethod(connect, IOGPU_SEL_NEW_RESOURCE,
	                               &resIn, sizeof(resIn),
	                               &resOut, &resOutSize);
	if (kr != kIOReturnSuccess || resOutSize < sizeof(resOut) || resOut.resourceId == 0 || resOut.cpuAddress == 0) {
		smoke_log("IOGPU-SMOKE: [3/7] selector 8 (NEW_RESOURCE with EXEC|NONCACHEABLE tail): FAIL (ret 0x%x, resId %u, cpuAddr 0x%llx)\n",
		          kr, resOut.resourceId, (unsigned long long)resOut.cpuAddress);
		failed_step = 3;
		goto cleanup;
	}
	resId = resOut.resourceId;
	resCpu = (uint8_t *)(uintptr_t)resOut.cpuAddress;
	smoke_log("IOGPU-SMOKE: [3/7] selector 8 (NEW_RESOURCE with EXEC|NONCACHEABLE tail): PASS (resId %u, GPU VA 0x%llx, CPU VA 0x%llx, ret 0x0)\n",
	          resId, (unsigned long long)resOut.gpuAddress, (unsigned long long)resOut.cpuAddress);

	/* ------------------------------------------------------------------
	 * Populate Compute Job Data & CS Words into user CPU mapping
	 * [refs/husky_iogpu_selftest.cpp:295-318; refs/husky_gpu_compute.h:20-60]
	 * ------------------------------------------------------------------ */
	/* 1. Job page at offset 0x0000 (GPU VA 0x06004000) */
	memcpy(resCpu, husky_comp_job, sizeof(husky_comp_job));

	/* 2. Input array at offset 0x1000 (GPU VA 0x06005000) */
	volatile uint32_t *inArr = (volatile uint32_t *)(resCpu + 0x1000);
	for (unsigned i = 0; i < HUSKY_COMP_THREADS; i++) {
		inArr[i] = comp_in(i);
	}

	/* 3. Output array poisoned at offset 0x2000 (GPU VA 0x06006000) */
	volatile uint32_t *outArr = (volatile uint32_t *)(resCpu + 0x2000);
	for (unsigned i = 0; i < HUSKY_COMP_THREADS; i++) {
		outArr[i] = 0xdeadbeefu;
	}

	/* 4. CS words chunk at offset 0x3000 (GPU VA 0x06007000) */
	memcpy(resCpu + 0x3000, husky_comp_cs, sizeof(husky_comp_cs));

	/* Commit memory writes before submission */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* ------------------------------------------------------------------
	 * [4/7] Selector 6: NEW_COMMAND_QUEUE
	 * [refs/iogpu_uc_abi.h:123, 235, 325-339; refs/IOGPUDeviceUserClient.cpp:272-301, 916]
	 * ------------------------------------------------------------------ */
	iogpu_new_command_queue_args_t qArgs;
	memset(&qArgs, 0, sizeof(qArgs));
	iogpu_new_command_queue_reply_t qReply;
	memset(&qReply, 0, sizeof(qReply));
	size_t qReplySize = sizeof(qReply);

	kr = IOConnectCallStructMethod(connect, IOGPU_SEL_NEW_COMMAND_QUEUE,
	                               &qArgs, sizeof(qArgs),
	                               &qReply, &qReplySize);
	if (kr != kIOReturnSuccess || qReplySize < sizeof(qReply) || qReply.queueId == 0) {
		smoke_log("IOGPU-SMOKE: [4/7] selector 6 (NEW_COMMAND_QUEUE): FAIL (ret 0x%x, queueId %llu)\n",
		          kr, (unsigned long long)qReply.queueId);
		failed_step = 4;
		goto cleanup;
	}
	queueId = (uint32_t)qReply.queueId;
	smoke_log("IOGPU-SMOKE: [4/7] selector 6 (NEW_COMMAND_QUEUE): PASS (queueId %u, ret 0x0)\n", queueId);

	/* ------------------------------------------------------------------
	 * Format S-Shmem and K-Shmem Framing
	 * [refs/husky_iogpu_selftest.cpp:342-389; refs/iogpu_uc_abi.h:427-500]
	 * ------------------------------------------------------------------ */
	/* S-Shmem: Block header (0x10) + Segment header (0x20) + Group entry (0x40) = 0x70 bytes */
	memset(sBytes, 0, 4096);
	iogpu_seg_block_header_t bh;
	memset(&bh, 0, sizeof(bh));
	bh.tag = 0x7e57;
	bh.count = 1;
	bh.control = IOGPU_SEG_CTRL_LAST | 0x70u;
	memcpy(sBytes, &bh, sizeof(bh));

	iogpu_segment_header_t seg;
	memset(&seg, 0, sizeof(seg));
	seg.kcStart = 0;
	seg.kcEnd = 0xac + 0x24; /* SetResourceGroups (0xac) + MALI_KCMD_SUBMIT_CS (0x24) = 0xd0 */
	seg.sidebandOff = 0;
	seg.nResources = 1;
	seg.nGroups = 1;
	memcpy(sBytes + 0x10, &seg, sizeof(seg));

	iogpu_resource_group_entry_t grp;
	memset(&grp, 0, sizeof(grp));
	grp.n = 1;
	grp.resId[0] = resId;
	grp.sizeKiB[0] = 32;
	grp.usage[0] = 3; /* ReadWrite */
	memcpy(sBytes + 0x30, &grp, sizeof(grp));

	/* K-Shmem: SetResourceGroups (0xac) + MALI_KCMD_SUBMIT_CS (0x24) */
	memset(kBytes, 0, 4096);
	iogpu_kcmd_header_t kh0;
	kh0.type = kIOGPUKCmd_SetResourceGroups; /* 15 */
	kh0.size = IOGPU_KCMD_SETGROUPS_SIZE;   /* 0xac */
	memcpy(kBytes, &kh0, sizeof(kh0));
	*(uint32_t *)(kBytes + sizeof(kh0)) = 1; /* numGroups = 1 */

	struct mali_kcmd_submit_cs sc;
	memset(&sc, 0, sizeof(sc));
	sc.header.type = MALI_KCMD_SUBMIT_CS;   /* 0x15 */
	sc.header.size = (uint32_t)(sizeof(struct mali_kcmd_submit_cs) + 4); /* 0x20 + 4 = 0x24 */
	sc.stream = HUSKY_COMP_JOB_VA + 0x3000; /* 0x06007000: 64-byte aligned CS chunk */
	sc.size = (uint32_t)sizeof(husky_comp_cs); /* 160 bytes: multiple of 8 */
	sc.flush_id = 0;                        /* unconditional leading flush */
	sc.n_wait = 0;
	sc.n_signal = 1;
	memcpy(kBytes + 0xac, &sc, sizeof(sc));
	*(uint32_t *)(kBytes + 0xac + sizeof(sc)) = 0; /* signal sync object 0 */

	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* ------------------------------------------------------------------
	 * [5/7] Submit through Trap 0 (SUBMIT_ONE)
	 * [refs/iogpu_uc_abi.h:180-183, 403-419; refs/IOGPUDeviceUserClient.cpp:1006-1050;
	 *  refs/IOGPUCommandQueue.cpp:77-98, 101-333]
	 * ------------------------------------------------------------------ */
	iogpu_cmdbuf_args_t cmdbuf;
	memset(&cmdbuf, 0, sizeof(cmdbuf));
	cmdbuf.kernelCommandShmemId = kShmemId;
	cmdbuf.segmentListShmemId   = sShmemId;
	cmdbuf.scheduledRef         = 0x1122334455667788ULL;
	cmdbuf.completedRef         = 0x8877665544332211ULL;

	uint32_t submitStatus = 0xdeadbeef;
	kr = smoke_trap4(connect, kIOGPUTrap_SubmitOne,
	                    (uintptr_t)queueId,
	                    (uintptr_t)sizeof(cmdbuf),
	                    (uintptr_t)&cmdbuf,
	                    (uintptr_t)&submitStatus);

	if (kr != kIOReturnSuccess || submitStatus != kIOGPUStatus_Success) {
		smoke_log("IOGPU-SMOKE: [5/7] trap 0 (SUBMIT_ONE): FAIL (ret 0x%x, status 0x%x)\n",
		          kr, submitStatus);
		failed_step = 5;
		goto cleanup;
	}
	smoke_log("IOGPU-SMOKE: [5/7] trap 0 (SUBMIT_ONE): PASS (status 0, CS job dispatched to Mali-G715, ret 0x0)\n");

	/* ------------------------------------------------------------------
	 * [6/7] Poll Sync Object / Hardware Completion
	 * [refs/IOGPUCommandQueue.cpp:301-318; refs/IOGPUDeviceUserClient.cpp:1038-1049]
	 * ------------------------------------------------------------------ */
	/*
	 * In kernel self-test (refs/husky_iogpu_selftest.cpp:418-425), the kernel peeks
	 * directly at IOGPUCommandQueue::syncValue() via internal pointers.
	 * Userland cannot access kernel C++ objects. However, in our driver:
	 * refs/IOGPUCommandQueue.cpp:301-318 (processOneCommandBuffer) performs a synchronous
	 * hardware poll loop:
	 *   for (us = 0; us < 500000; us += 10) {
	 *       if (mali_backend_ops.syncobj_read(fSync) >= target) break;
	 *       IODelay(10);
	 *   }
	 * and sets status = kIOGPUStatus_ParseError (0xa) on timeout.
	 * The submission trap (refs/IOGPUDeviceUserClient.cpp:1039-1047) only returns
	 * to userland after this wait completes and copyout()s status to &submitStatus.
	 * Thus, observing submitStatus == kIOGPUStatus_Success (0) and kr == kIOReturnSuccess (0)
	 * is the authoritative, verified userland proof that hardware sync target 1 was reached.
	 */
	bool syncOk = (submitStatus == kIOGPUStatus_Success);
	if (!syncOk) {
		smoke_log("IOGPU-SMOKE: [6/7] poll sync object / completion: FAIL (status 0x%x, ret 0x%x)\n",
		          submitStatus, kr);
		failed_step = 6;
		goto cleanup;
	}
	smoke_log("IOGPU-SMOKE: [6/7] poll sync object / completion: PASS (status 0, kernel verified sync target 1, ret 0x0)\n");

	/* ------------------------------------------------------------------
	 * [7/7] Verify All 256 Results (out[i] == in[i] * 3 + 7)
	 * [refs/husky_iogpu_selftest.cpp:432-461; refs/IOGPUMemory.cpp:178-204]
	 * ------------------------------------------------------------------ */
	/*
	 * cpuAddress returned by selector 8 (refs/IOGPUMemory.cpp:178-204) is mapped
	 * into client task (dev->owner()) via IOMemoryDescriptor::createMappingInTask
	 * by physical address, write-combined (xnu 0112).
	 * Memory barrier ensures CPU read reflects hardware writes.
	 */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	unsigned good = 0;
	unsigned firstBad = ~0u;
	for (unsigned i = 0; i < HUSKY_COMP_THREADS; i++) {
		uint32_t inVal = comp_in(i);
		uint32_t want = inVal * HUSKY_COMP_MUL + HUSKY_COMP_ADD;
		uint32_t got = outArr[i];
		if (got == want) {
			good++;
		} else if (firstBad == ~0u) {
			firstBad = i;
		}
	}

	if (good != HUSKY_COMP_THREADS) {
		smoke_log("IOGPU-SMOKE: [7/7] compute verification: FAIL (%u/%u correct; first bad [%u]=0x%x, want 0x%x, ret 0x0)\n",
		          good, HUSKY_COMP_THREADS, firstBad, outArr[firstBad],
		          comp_in(firstBad) * HUSKY_COMP_MUL + HUSKY_COMP_ADD);
		failed_step = 7;
		goto cleanup;
	}
	smoke_log("IOGPU-SMOKE: [7/7] compute verification: PASS (%u/%u correct, out[i]=in[i]*3+7, ret 0x0)\n",
	          good, HUSKY_COMP_THREADS);

cleanup:
	smoke_log("IOGPU-SMOKE: starting teardown and cleanup in reverse order...\n");
	if (connect != IO_OBJECT_NULL) {
		/* Delete command queue via Selector 7 (DELETE_COMMAND_QUEUE) [refs/iogpu_uc_abi.h:124; refs/IOGPUDeviceUserClient.cpp:304-315] */
		if (queueId != 0) {
			smoke_log("IOGPU-SMOKE: cleanup: deleting command queue %u via selector 7...\n", queueId);
			uint64_t sInQ[1] = { (uint64_t)queueId };
			kern_return_t cl_kr = IOConnectCallScalarMethod(connect, kIOGPUSel_DeleteCommandQueue,
			                                                sInQ, 1, NULL, NULL);
			smoke_log("IOGPU-SMOKE: cleanup: selector 7 delete queue %u ret 0x%x\n", queueId, cl_kr);
			queueId = 0;
		}

		/* Release resource via Trap 1 (RELEASE_RESOURCE) [refs/iogpu_uc_abi.h:184; refs/IOGPUDeviceUserClient.cpp:1053-1067] */
		if (resId != 0) {
			smoke_log("IOGPU-SMOKE: cleanup: releasing resource %u via trap 1 (RELEASE_RESOURCE)...\n", resId);
			kern_return_t cl_kr = smoke_trap1(connect, kIOGPUTrap_ReleaseResource, (uintptr_t)resId);
			smoke_log("IOGPU-SMOKE: cleanup: trap 1 release resource %u ret 0x%x\n", resId, cl_kr);
			resId = 0;
		}

		/* Destroy shmems via Selector 13 (DESTROY_DEVICE_SHMEM) [refs/iogpu_uc_abi.h:130; refs/IOGPUDeviceUserClient.cpp:453-463] */
		if (sShmemId != 0) {
			smoke_log("IOGPU-SMOKE: cleanup: destroying S-shmem %u via selector 13...\n", sShmemId);
			uint64_t sInSShmem[1] = { (uint64_t)sShmemId };
			kern_return_t cl_kr = IOConnectCallScalarMethod(connect, kIOGPUSel_DestroyDeviceShmem,
			                                                sInSShmem, 1, NULL, NULL);
			smoke_log("IOGPU-SMOKE: cleanup: selector 13 destroy S-shmem %u ret 0x%x\n", sShmemId, cl_kr);
			sShmemId = 0;
		}
		if (kShmemId != 0) {
			smoke_log("IOGPU-SMOKE: cleanup: destroying K-shmem %u via selector 13...\n", kShmemId);
			uint64_t sInKShmem[1] = { (uint64_t)kShmemId };
			kern_return_t cl_kr = IOConnectCallScalarMethod(connect, kIOGPUSel_DestroyDeviceShmem,
			                                                sInKShmem, 1, NULL, NULL);
			smoke_log("IOGPU-SMOKE: cleanup: selector 13 destroy K-shmem %u ret 0x%x\n", kShmemId, cl_kr);
			kShmemId = 0;
		}

		smoke_log("IOGPU-SMOKE: cleanup: closing user-client connection 0x%x...\n", connect);
		IOServiceClose(connect);
		connect = IO_OBJECT_NULL;
	}

	if (service != IO_OBJECT_NULL) {
		IOObjectRelease(service);
		service = IO_OBJECT_NULL;
	}

	if (failed_step == 0) {
		smoke_log("IOGPU-SMOKE: ALL 7 STEPS PASSED\n");
	} else {
		smoke_log("IOGPU-SMOKE: FAILED at step %d\n", failed_step);
	}

	smoke_log_close();
	return (failed_step == 0) ? 0 : 1;
}
