/* SPDX-License-Identifier: MIT
 * Link/run smoke test for the Mesa arch-11 library set (libmali_mesa.a). ORIGINAL code.
 *
 * Exercises one entry point per spec 06 s4 component so a successful link proves the closure is
 * complete (no DRM, no unresolved Mesa symbol) and a successful run proves the generated packers
 * and the Valhall back end work on the build host:
 *   P1 genxml v11 packers  : pack/unpack a Resource descriptor, round trip
 *   P2 cs_builder          : MOVE32 + MOVE48 + WAIT + CALL + ERROR_BARRIER, root size > 0
 *                            asserts opcodes match mali_backend_ops.h (MALI_CS_*)
 *   P3 model               : GPU_ID 0xb8a21020 (arch 11.8, prod major 2), variant 4, 7 cores -> "G715"
 *                            (spec 06 s2: gpu_id decode, M:model/pan_model.c:109-115)
 *   P7/P9 NIR + Valhall    : NIR compute kernel (store to global) -> pan_shader_compile -> binary
 *   ABI Assertions         : HAVE_FUNC_ATTRIBUTE_PACKED defined, sizeof(struct pan_varying_layout) == 152
 *
 * Prints one "PASS:/FAIL:" line per stage; exit status = number of failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifndef PAN_ARCH
#error "build with -DPAN_ARCH=11"
#endif

/* Include Mesa headers FIRST */
#include "genxml/gen_macros.h"
#include "genxml/cs_builder.h"
#include "model/pan_model.h"
#include "compiler/pan_compiler.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "lib/pan_props.h"
#include "util/u_dynarray.h"

/* Include contract header AFTER Mesa headers to verify include-order compatibility */
#include "contract/mali_backend_ops.h"
#include "mali_mesa_abi_check.h"

static int failures;
#define CHECK(cond, ...)                                                   \
   do {                                                                    \
      if (cond) {                                                          \
         printf("PASS: ");                                                 \
      } else {                                                             \
         printf("FAIL: ");                                                 \
         failures++;                                                       \
      }                                                                    \
      printf(__VA_ARGS__);                                                 \
      printf("\n");                                                        \
   } while (0)

/* ---- ABI assertions -------------------------------------------------------------------- */
static void
test_abi(void)
{
   bool packed_ok = false;
#ifdef HAVE_FUNC_ATTRIBUTE_PACKED
   packed_ok = true;
#endif
   CHECK(packed_ok, "HAVE_FUNC_ATTRIBUTE_PACKED defined for compiler ABI");
   CHECK(sizeof(struct pan_varying_layout) == 152,
         "sizeof(struct pan_varying_layout) == 152 (actual: %zu)", sizeof(struct pan_varying_layout));
   CHECK(sizeof(struct pan_varying_slot) == 4,
         "sizeof(struct pan_varying_slot) == 4 (actual: %zu)", sizeof(struct pan_varying_slot));
   CHECK(sizeof(enum pan_varying_knowledge) == 1,
         "sizeof(enum pan_varying_knowledge) == 1 (actual: %zu)", sizeof(enum pan_varying_knowledge));
}

/* ---- P1 -------------------------------------------------------------------------------- */
static void
test_packers(void)
{
   struct mali_resource_packed packed;
   pan_pack(&packed, RESOURCE, cfg) {
      cfg.address = 0x1234567000ull;
      cfg.size = 0x2a0;
   }
   struct MALI_RESOURCE back;
   MALI_RESOURCE_unpack(&packed, &back);
   CHECK(back.address == 0x1234567000ull && back.size == 0x2a0,
         "genxml v11 RESOURCE pack/unpack (addr=0x%llx size=0x%llx)",
         (unsigned long long)back.address, (unsigned long long)back.size);
}

/* ---- P2 -------------------------------------------------------------------------------- */
static uint64_t cs_storage[512];
static struct cs_buffer
no_more_chunks(void *cookie)
{
   (void)cookie;
   return (struct cs_buffer){0}; /* chunk overflow would invalidate the builder: we stay in one */
}

static void
test_cs_builder(void)
{
   struct cs_builder b;
   struct cs_builder_conf conf = {
      .nr_registers = 96,          /* spec 06 s3.4: r0..r91 user, r92..95 chunk link/wrapper */
      .nr_kernel_registers = 4,
      .alloc_buffer = no_more_chunks,
      .ls_sb_slot = 0,
   };
   struct cs_buffer root = {
      .cpu = cs_storage, .gpu = 0x80000000ull, .capacity = ARRAY_SIZE(cs_storage),
   };
   cs_builder_init(&b, &conf, root);
   cs_move32_to(&b, cs_reg32(&b, 10), 0x12345678);
   cs_move48_to(&b, cs_reg64(&b, 12), 0xdeadbeef00ull);
   cs_wait_slot(&b, 0);
   cs_call(&b, cs_reg64(&b, 80), cs_reg32(&b, 82));
   cs_error_barrier(&b);
   cs_end(&b);

   uint32_t sz = cs_is_valid(&b) ? cs_root_chunk_size(&b) : 0;
   /* every CS instruction is 8 bytes; 5 instructions emitted */
   CHECK(cs_is_valid(&b) && sz >= 5 * 8 && sz % 8 == 0 && cs_root_chunk_gpu_addr(&b) == 0x80000000ull,
         "cs_builder chunk: %u bytes (%u instructions) at gpu 0x%llx", sz, sz / 8,
         (unsigned long long)cs_root_chunk_gpu_addr(&b));

   /* MOVE32 r10 encoding: opcode byte 2 in bits 56..63, dest in 48..55, imm in low 32 */
   CHECK(cs_storage[0] == MALI_CS_MOVE32(10, 0x12345678),
         "MOVE32 matches contract macro MALI_CS_MOVE32: 0x%016llx", (unsigned long long)cs_storage[0]);

   /* MOVE48 r12 encoding: opcode byte 1 in bits 56..63, dest in 48..55, imm in low 48 */
   CHECK(cs_storage[1] == MALI_CS_MOVE48(12, 0xdeadbeef00ull),
         "MOVE48 matches contract macro MALI_CS_MOVE48: 0x%016llx", (unsigned long long)cs_storage[1]);

   /* WAIT slot 0 encoding: opcode byte 3 in bits 56..63, mask bit 0 in bit 16 */
   CHECK(cs_storage[2] == MALI_CS_WAIT(1),
         "WAIT matches contract macro MALI_CS_WAIT: 0x%016llx", (unsigned long long)cs_storage[2]);

   /* CALL r80, r82 encoding: opcode byte 32 in bits 56..63 */
   CHECK(cs_storage[3] == MALI_CS_CALL(80, 82),
         "CALL matches contract macro MALI_CS_CALL: 0x%016llx", (unsigned long long)cs_storage[3]);

   /* ERROR_BARRIER encoding: opcode byte 47 in bits 56..63 */
   CHECK(cs_storage[4] == MALI_CS_ERROR_BARRIER,
         "ERROR_BARRIER matches contract macro MALI_CS_ERROR_BARRIER: 0x%016llx", (unsigned long long)cs_storage[4]);

   /* Verify contract opcodes for FLUSH_CACHE2 (36), SYNC_ADD64 (51), STORE_MULTIPLE (21) */
   CHECK((MALI_CS_FLUSH_CACHE2(94) >> 56) == 36,
         "FLUSH_CACHE2 opcode 36 matches v11.xml: 0x%016llx", (unsigned long long)MALI_CS_FLUSH_CACHE2(94));
   CHECK((MALI_CS_SYNC_ADD64(0x20, 1) >> 56) == 51,
         "SYNC_ADD64 opcode 51 matches v11.xml: 0x%016llx", (unsigned long long)MALI_CS_SYNC_ADD64(0x20, 1));
   CHECK((MALI_CS_STORE_MULTIPLE(10, 20, 0xf, 0) >> 56) == 21,
         "STORE_MULTIPLE opcode 21 matches v11.xml: 0x%016llx", (unsigned long long)MALI_CS_STORE_MULTIPLE(10, 20, 0xf, 0));

   cs_builder_fini(&b);
}

/* ---- P3 -------------------------------------------------------------------------------- */
static const struct pan_model *g715;

static void
test_model(void)
{
   g715 = pan_get_model(0xb8a21020ull, 4, 7, 0);
   CHECK(g715 && g715->name && strstr(g715->name, "G715"), "pan_get_model(0xb8a21020, variant 4, 7 cores) -> %s",
         g715 && g715->name ? g715->name : "(null)");
}

/* ---- P7 / P9 ---------------------------------------------------------------------------- */
static void
test_valhall_compile(void)
{
   glsl_type_singleton_init_or_ref();

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, pan_get_nir_shader_compiler_options(PAN_ARCH, MESA_SHADER_COMPUTE, false),
      "mali_mesa_smoke");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.workgroup_size_variable = false;

   /* *(uint32 *)(0xdead0000 + local_invocation_index * 4) = local_invocation_index + 1 */
   nir_def *idx = nir_load_local_invocation_index(&b);
   nir_def *addr = nir_iadd(&b, nir_imm_int64(&b, 0xdead0000ll),
                            nir_u2u64(&b, nir_imul_imm(&b, idx, 4)));
   nir_store_global(&b, nir_iadd_imm(&b, idx, 1), addr, .write_mask = 0x1, .align_mul = 4);

   struct pan_compile_inputs inputs = {
      .arch = PAN_ARCH,
      .model = g715,
   };
   struct pan_shader_info info;
   memset(&info, 0, sizeof(info));

   /* local_invocation_index is not a hardware sysval on Valhall: lower it to ids (panvk does the same) */
   nir_lower_compute_system_values(b.shader, NULL);
   pan_preprocess_nir(b.shader, PAN_ARCH);
   pan_postprocess_nir(b.shader, &inputs, &info);

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);
   pan_shader_compile(b.shader, &inputs, &binary, &info);

   CHECK(binary.size >= 8 && binary.size % 8 == 0,
         "Valhall compile (arch %d): %u byte binary, %u work regs", PAN_ARCH,
         (unsigned)binary.size, (unsigned)info.work_reg_count);

   util_dynarray_fini(&binary);
   ralloc_free(b.shader);
   glsl_type_singleton_decref();
}

int
main(void)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   test_abi();
   test_packers();
   test_cs_builder();
   test_model();
   if (g715)
      test_valhall_compile();
   printf("%s (%d failure%s)\n", failures ? "SMOKE FAILED" : "SMOKE OK", failures,
          failures == 1 ? "" : "s");
   return failures;
}
