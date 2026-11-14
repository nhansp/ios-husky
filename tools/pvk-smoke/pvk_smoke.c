/*
 * pvk_smoke: exercise the kernel's panthor interface (HUSKY_PNT_CONNECT_TYPE on MaliAccelerator) the way panvk
 * will: device queries, a VM with user-placed BOs, groups with one and three queues, CS jobs that store values,
 * a cross-queue SYNC_WAIT64, and binary/timeline syncobjs. Prints PASS/FAIL lines to the console.
 */
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "husky_panthor_drm.h"

#define CS_MOVE48(r, imm)       ((1ULL << 56) | ((uint64_t)(r) << 48) | ((uint64_t)(imm) & 0xffffffffffffULL))
#define CS_MOVE32(r, imm)       ((2ULL << 56) | ((uint64_t)(r) << 48) | (uint32_t)(imm))
#define CS_STORE32(src, addr)   ((21ULL << 56) | ((uint64_t)(src) << 48) | ((uint64_t)(addr) << 40) | (1ULL << 16))

static io_connect_t conn;
static uint32_t sel_base;      /* 0 on HuskyPanthorUserClient, HUSKY_PNT_TUNNEL_BASE on IOGPUDeviceUserClient */
static int failures;

static int
pnt_ioctl(uint32_t req, void *arg, size_t size)
{
	uint64_t in = req, err = 0;
	uint32_t nout = 1;
	size_t osize = size;
	kern_return_t kr = IOConnectCallMethod(conn, sel_base + HUSKY_PNT_SEL_IOCTL, &in, 1, arg, size, &err, &nout, arg, &osize);
	if (kr != KERN_SUCCESS) {
		printf("pvk_smoke: ioctl 0x%x: IOConnectCallMethod 0x%x\n", req, kr);
		return EIO;
	}
	return (int)err;
}

static void *
pnt_mmap(uint32_t handle)
{
	uint64_t in = handle, out[2] = { 0, 0 };
	uint32_t nout = 2;
	if (IOConnectCallScalarMethod(conn, sel_base + HUSKY_PNT_SEL_MMAP, &in, 1, out, &nout) != KERN_SUCCESS) {
		return NULL;
	}
	return (void *)(uintptr_t)out[0];
}

#define CHECK(cond, ...) do { \
	if (cond) { printf("pvk_smoke: PASS " __VA_ARGS__); printf("\n"); } \
	else { failures++; printf("pvk_smoke: FAIL " __VA_ARGS__); printf(" (line %d)\n", __LINE__); } \
} while (0)

static uint32_t
bo_create(uint64_t size, uint64_t *out_size)
{
	struct drm_panthor_bo_create a = { .size = size };
	int e = pnt_ioctl(DRM_IOCTL_PANTHOR_BO_CREATE, &a, sizeof(a));
	CHECK(e == 0, "BO_CREATE 0x%llx -> handle %u size 0x%llx (%d)", size, a.handle, a.size, e);
	if (out_size) {
		*out_size = a.size;
	}
	return a.handle;
}

static int
vm_map(uint32_t vm, uint32_t bo, uint64_t va, uint64_t size)
{
	struct drm_panthor_vm_bind_op op = {
		.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = bo, .bo_offset = 0, .va = va, .size = size,
	};
	struct drm_panthor_vm_bind b = { .vm_id = vm, .ops = DRM_PANTHOR_OBJ_ARRAY(1, &op) };
	return pnt_ioctl(DRM_IOCTL_PANTHOR_VM_BIND, &b, sizeof(b));
}

static uint32_t
syncobj_create(uint32_t flags)
{
	struct drm_syncobj_create a = { .flags = flags };
	int e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_CREATE, &a, sizeof(a));
	return e == 0 ? a.handle : 0;
}

static int
timeline_wait(uint32_t h, uint64_t point, int64_t timeout_ns)
{
	struct drm_syncobj_timeline_wait w = {
		.handles = (uint64_t)(uintptr_t)&h, .points = (uint64_t)(uintptr_t)&point, .timeout_nsec = timeout_ns,
		.count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
	};
	return pnt_ioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w, sizeof(w));
}

static uint64_t
timeline_query(uint32_t h)
{
	uint64_t point = 0;
	struct drm_syncobj_timeline_array q = { .handles = (uint64_t)(uintptr_t)&h, .points = (uint64_t)(uintptr_t)&point,
		.count_handles = 1 };
	return pnt_ioctl(DRM_IOCTL_SYNCOBJ_QUERY, &q, sizeof(q)) == 0 ? point : UINT64_MAX;
}

static uint32_t
group_create(uint32_t vm, uint32_t nq, uint64_t shader_present)
{
	struct drm_panthor_queue_create q[3];
	for (uint32_t i = 0; i < nq; i++) {
		q[i] = (struct drm_panthor_queue_create){ .priority = 1, .ringbuf_size = 64 * 1024 };
	}
	const uint8_t cores = (uint8_t)__builtin_popcountll(shader_present);
	struct drm_panthor_group_create g = {
		.queues = DRM_PANTHOR_OBJ_ARRAY(nq, q), .max_compute_cores = cores, .max_fragment_cores = cores,
		.max_tiler_cores = 1, .priority = PANTHOR_GROUP_PRIORITY_MEDIUM, .compute_core_mask = shader_present,
		.fragment_core_mask = shader_present, .tiler_core_mask = 1, .vm_id = vm,
	};
	int e = pnt_ioctl(DRM_IOCTL_PANTHOR_GROUP_CREATE, &g, sizeof(g));
	CHECK(e == 0, "GROUP_CREATE %u queues -> %u (%d)", nq, g.group_handle, e);
	return g.group_handle;
}

/* One job on queue qi: wait (optional), run stream, signal (syncobj, point). */
static int
submit(uint32_t group, uint32_t qi, uint64_t stream_va, uint32_t stream_size, uint32_t wait_h, uint64_t wait_pt,
    uint32_t sig_h, uint64_t sig_pt)
{
	struct drm_panthor_sync_op ops[2];
	uint32_t n = 0;
	if (wait_h) {
		ops[n++] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ |
			DRM_PANTHOR_SYNC_OP_WAIT, .handle = wait_h, .timeline_value = wait_pt };
	}
	ops[n++] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ |
		DRM_PANTHOR_SYNC_OP_SIGNAL, .handle = sig_h, .timeline_value = sig_pt };
	struct drm_panthor_queue_submit qs = {
		.queue_index = qi, .stream_size = stream_size, .stream_addr = stream_va, .latest_flush = 0,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(n, ops),
	};
	struct drm_panthor_group_submit s = { .group_handle = group, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs) };
	return pnt_ioctl(DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &s, sizeof(s));
}

/* A stream at words[0..]: store value to va. Returns its size in bytes. */
static uint32_t
emit_store(uint64_t *words, uint64_t va, uint32_t value)
{
	uint32_t n = 0;
	words[n++] = CS_MOVE48(0, va);
	words[n++] = CS_MOVE32(2, value);
	words[n++] = CS_STORE32(2, 0);
	return n * 8;
}

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaliAccelerator"));
	if (svc == IO_OBJECT_NULL) {
		printf("pvk_smoke: FAIL no MaliAccelerator\n");
		return 1;
	}
	/* the dedicated client answers a device query; the suite runs over the IOGPU tunnel, which panvk uses */
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), HUSKY_PNT_CONNECT_TYPE, &conn);
	CHECK(kr == KERN_SUCCESS, "IOServiceOpen type 0x%x (0x%x)", HUSKY_PNT_CONNECT_TYPE, kr);
	if (kr == KERN_SUCCESS) {
		struct drm_panthor_dev_query q0 = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO };
		int e0 = pnt_ioctl(DRM_IOCTL_PANTHOR_DEV_QUERY, &q0, sizeof(q0));
		CHECK(e0 == 0 && q0.size != 0, "dedicated client: GPU_INFO size %u (%d)", q0.size, e0);
		IOServiceClose(conn);
	}
	kr = IOServiceOpen(svc, mach_task_self(), 1, &conn);
	sel_base = HUSKY_PNT_TUNNEL_BASE;
	CHECK(kr == KERN_SUCCESS, "IOServiceOpen IOGPU device client (0x%x)", kr);
	if (kr != KERN_SUCCESS) {
		return 1;
	}

	struct drm_panthor_gpu_info gpu = { 0 };
	struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO, .size = sizeof(gpu),
		.pointer = (uint64_t)(uintptr_t)&gpu };
	int e = pnt_ioctl(DRM_IOCTL_PANTHOR_DEV_QUERY, &q, sizeof(q));
	CHECK(e == 0 && gpu.gpu_id >> 16 == 0xb8a2, "GPU_INFO gpu_id 0x%x csf 0x%x shader 0x%llx tiler 0x%llx l2 0x%llx "
	    "mmu 0x%x thread 0x%x/0x%x (%d)", gpu.gpu_id, gpu.csf_id, gpu.shader_present, gpu.tiler_present, gpu.l2_present,
	    gpu.mmu_features, gpu.thread_features, gpu.max_threads, e);
	struct drm_panthor_csif_info csif = { 0 };
	q = (struct drm_panthor_dev_query){ .type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO, .size = sizeof(csif),
		.pointer = (uint64_t)(uintptr_t)&csif };
	e = pnt_ioctl(DRM_IOCTL_PANTHOR_DEV_QUERY, &q, sizeof(q));
	CHECK(e == 0 && csif.cs_reg_count == 96, "CSIF_INFO csg %u cs %u regs %u sb %u unpreserved %u (%d)",
	    csif.csg_slot_count, csif.cs_slot_count, csif.cs_reg_count, csif.scoreboard_slot_count,
	    csif.unpreserved_cs_reg_count, e);
	struct drm_panthor_timestamp_info ts = { 0 };
	q = (struct drm_panthor_dev_query){ .type = DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO, .size = sizeof(ts),
		.pointer = (uint64_t)(uintptr_t)&ts };
	e = pnt_ioctl(DRM_IOCTL_PANTHOR_DEV_QUERY, &q, sizeof(q));
	CHECK(e == 0 && ts.timestamp_frequency != 0, "TIMESTAMP_INFO freq %llu now %llu (%d)", ts.timestamp_frequency,
	    ts.current_timestamp, e);

	struct drm_panthor_vm_create vc = { .user_va_range = 1ull << 47 };
	e = pnt_ioctl(DRM_IOCTL_PANTHOR_VM_CREATE, &vc, sizeof(vc));
	CHECK(e == 0, "VM_CREATE -> %u user 0x%llx (%d)", vc.id, vc.user_va_range, e);

	const uint64_t STREAM_VA = 0x10000000, DATA_VA = 0x100000000ull;
	uint64_t ssize = 0, dsize = 0;
	uint32_t sbo = bo_create(0x4000, &ssize), dbo = bo_create(0x10000, &dsize);
	uint64_t *stream = pnt_mmap(sbo);
	volatile uint32_t *data = pnt_mmap(dbo);
	CHECK(stream != NULL && data != NULL, "mmap stream %p data %p", (void *)stream, (void *)data);
	if (stream == NULL || data == NULL) {
		return 1;
	}
	e = vm_map(vc.id, sbo, STREAM_VA, ssize);
	CHECK(e == 0, "VM_BIND map stream (%d)", e);
	e = vm_map(vc.id, dbo, DATA_VA, dsize);
	CHECK(e == 0, "VM_BIND map data (%d)", e);

	/* 1: one queue, one job, timeline signal + CPU wait */
	uint32_t g1 = group_create(vc.id, 1, gpu.shader_present);
	uint32_t tl = syncobj_create(0);
	uint32_t sz = emit_store(stream, DATA_VA, 0xdeadbeef);
	double t0 = now_ms();
	e = submit(g1, 0, STREAM_VA, sz, 0, 0, tl, 1);
	CHECK(e == 0, "GROUP_SUBMIT single (%d)", e);
	e = timeline_wait(tl, 1, 1000000000ll);
	double t1 = now_ms();
	CHECK(e == 0 && data[0] == 0xdeadbeef, "job 1 stored 0x%x, wait %d in %.2f ms", data[0], e, t1 - t0);
	CHECK(timeline_query(tl) == 1, "QUERY point %llu", timeline_query(tl));

	/* 2: 100 jobs back to back: completion latency and ring wrap (64 KiB ring) */
	t0 = now_ms();
	for (uint32_t i = 0; i < 100 && e == 0; i++) {
		sz = emit_store(stream + 8 * (1 + i % 8), DATA_VA + 4, 0x1000 + i);
		e = submit(g1, 0, STREAM_VA + 64 * (1 + i % 8), sz, 0, 0, tl, 2 + i);
	}
	e = e ? e : timeline_wait(tl, 101, 2000000000ll);
	t1 = now_ms();
	CHECK(e == 0 && data[1] == 0x1000 + 99, "100 jobs: last value 0x%x, %.2f ms (%d)", data[1], t1 - t0, e);

	/* 3: three queues; queue 1 waits (GPU SYNC_WAIT64) on queue 0's point */
	uint32_t g3 = group_create(vc.id, 3, gpu.shader_present);
	uint32_t ta = syncobj_create(0), tb = syncobj_create(0);
	sz = emit_store(stream + 8 * 10, DATA_VA + 8, 0xa0a0a0a0);
	e = submit(g3, 0, STREAM_VA + 64 * 10, sz, 0, 0, ta, 1);
	CHECK(e == 0, "submit queue 0 (%d)", e);
	sz = emit_store(stream + 8 * 11, DATA_VA + 12, 0xb0b0b0b0);
	e = submit(g3, 1, STREAM_VA + 64 * 11, sz, ta, 1, tb, 1);
	CHECK(e == 0, "submit queue 1 waiting on queue 0 (%d)", e);
	e = timeline_wait(tb, 1, 1000000000ll);
	CHECK(e == 0 && data[2] == 0xa0a0a0a0 && data[3] == 0xb0b0b0b0, "cross-queue: 0x%x 0x%x (%d)", data[2], data[3], e);

	/* 4: syncobj CPU ops: signal, reset, transfer, wait-for-submit timeout */
	uint32_t b = syncobj_create(DRM_SYNCOBJ_CREATE_SIGNALED);
	struct drm_syncobj_wait bw = { .handles = (uint64_t)(uintptr_t)&b, .count_handles = 1, .timeout_nsec = 0 };
	e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_WAIT, &bw, sizeof(bw));
	CHECK(e == 0, "binary created signaled waits (%d)", e);
	struct drm_syncobj_array ra = { .handles = (uint64_t)(uintptr_t)&b, .count_handles = 1 };
	e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_RESET, &ra, sizeof(ra));
	bw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	bw.timeout_nsec = 5000000;
	e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_WAIT, &bw, sizeof(bw));
	CHECK(e == ETIME, "reset binary times out (%d)", e);
	struct drm_syncobj_transfer tr = { .src_handle = tl, .dst_handle = b, .src_point = 101, .dst_point = 0 };
	e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_TRANSFER, &tr, sizeof(tr));
	bw.flags = 0;
	e = e ? e : pnt_ioctl(DRM_IOCTL_SYNCOBJ_WAIT, &bw, sizeof(bw));
	CHECK(e == 0, "transfer timeline 101 -> binary, wait (%d)", e);
	uint64_t pt = 7;
	struct drm_syncobj_timeline_array sa = { .handles = (uint64_t)(uintptr_t)&ta, .points = (uint64_t)(uintptr_t)&pt,
		.count_handles = 1 };
	e = pnt_ioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sa, sizeof(sa));
	CHECK(e == 0 && timeline_query(ta) == 7, "CPU timeline signal 7 -> query %llu (%d)", timeline_query(ta), e);

	struct drm_panthor_group_get_state gs = { .group_handle = g3 };
	e = pnt_ioctl(DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &gs, sizeof(gs));
	CHECK(e == 0 && gs.state == 0, "group state 0x%x fatal 0x%x (%d)", gs.state, gs.fatal_queues, e);

	struct drm_panthor_group_destroy gd = { .group_handle = g3 };
	CHECK(pnt_ioctl(DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd, sizeof(gd)) == 0, "GROUP_DESTROY 3");
	gd.group_handle = g1;
	CHECK(pnt_ioctl(DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd, sizeof(gd)) == 0, "GROUP_DESTROY 1");
	IOServiceClose(conn);
	printf("pvk_smoke: DONE %d failures\n", failures);
	return failures != 0;
}
