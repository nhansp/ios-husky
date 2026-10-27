# SPDX-License-Identifier: MIT
# mesa_defs.cmake: Canonical preprocessor definitions for Mali Mesa (PAN_ARCH=11)
#
# Derived from compile_commands.json of Mesa 27.0.0-devel (commit 1f9035d9)
# configured with:
#   -Dbuildtype=release -Dtools=panfrost -Dgallium-drivers=softpipe
#   -Dopengl=true -Dplatforms= -Dllvm=disabled -Dshader-cache=disabled
#   -Dzlib=disabled -Dzstd=disabled
#
# ONE canonical define list consumed by:
#   1. CMake build of libmali_mesa.a (CMakeLists.txt)
#   2. The Metal plug-in (via generated mesa_defs.txt)
#   3. CI build scripts (via generated mesa_defs.txt)
#   4. Host tests (via generated mesa_defs.mk)
#
# ABI CRITICAL:
#   HAVE_FUNC_ATTRIBUTE_PACKED is required on all platforms. Without it,
#   enum pan_varying_knowledge defaults to 4 bytes instead of 1, adding
#   padding and altering the size (160 vs 152) and field offsets of
#   struct pan_varying_layout. Both Linux and Darwin clang/gcc define it.

# Common definitions applicable to all targets (Darwin/iOS and Linux)
set(MESA_DEFS_COMMON
  -D_FILE_OFFSET_BITS=64
  -D__STDC_CONSTANT_MACROS
  -D__STDC_FORMAT_MACROS
  -D__STDC_LIMIT_MACROS
  -DPACKAGE_VERSION=\"27.0.0-devel\"
  -DMESA_DEBUG=0
  -DTHREAD_SANITIZER=0
  -DUSE_GCC_ATOMIC_BUILTINS
  -DHAVE_UINT128
  -DHAVE_STRUCT_TIMESPEC
  -DHAVE_PTHREAD
  -DHAVE_DIRENT_D_TYPE
  -DHAVE_GFX_COMPUTE
  -DUSE_LIBGLVND=0
  -DHAVE_OPENGL=0
  -DHAVE_OPENGL_ES_1=0
  -DHAVE_OPENGL_ES_2=0
  -DHAVE___BUILTIN_BSWAP32
  -DHAVE___BUILTIN_BSWAP64
  -DHAVE___BUILTIN_CLZ
  -DHAVE___BUILTIN_CLZLL
  -DHAVE___BUILTIN_CTZ
  -DHAVE___BUILTIN_EXPECT
  -DHAVE___BUILTIN_FFS
  -DHAVE___BUILTIN_FFSLL
  -DHAVE___BUILTIN_POPCOUNT
  -DHAVE___BUILTIN_POPCOUNTLL
  -DHAVE___BUILTIN_UNREACHABLE
  -DHAVE___BUILTIN_TYPES_COMPATIBLE_P
  -DHAVE___BUILTIN_ADD_OVERFLOW
  -DHAVE_FUNC_ATTRIBUTE_CONST
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN
  -DHAVE_FUNC_ATTRIBUTE_MALLOC
  -DHAVE_FUNC_ATTRIBUTE_PURE
  -DHAVE_FUNC_ATTRIBUTE_UNUSED
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT
  -DHAVE_FUNC_ATTRIBUTE_WEAK
  -DHAVE_FUNC_ATTRIBUTE_FORMAT
  -DHAVE_FUNC_ATTRIBUTE_PACKED
  -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL
  -DHAVE_FUNC_ATTRIBUTE_NORETURN
  -DHAVE_FUNC_ATTRIBUTE_COLD
  -DHAVE_FUNC_ATTRIBUTE_VISIBILITY
  -DHAVE_FMEMOPEN
  -DHAVE_POSIX_MEMALIGN
  -DHAVE_STRTOK_R
  -DHAVE_STRTOF
  -DHAVE_MKOSTEMP
  -DHAVE_FLOCK
  -DHAVE_SYSCONF
  -DHAVE_DLADDR
  -DHAVE_DLFCN_H
  -DHAVE_POLL_H
  -DHAVE_STRTOD_L
  -DUTIL_ARCH_LITTLE_ENDIAN=1
  -DUTIL_ARCH_BIG_ENDIAN=0
)

# Darwin/iOS specific:
# - _DARWIN_C_SOURCE: required for Darwin/BSD libc extensions under -std=c11 (__STRICT_ANSI__)
# - HAVE_REALLOCARRAY omitted: absent in Apple libSystem (Mesa uses inline fallback in util/reallocarray.h)
# - HAVE_SC_PHYS_PAGES omitted: sysconf(_SC_PHYS_PAGES) absent on Darwin; os_misc.c:379 uses sysctl(CTL_HW, HW_MEMSIZE)
# - HAVE_FUNC_ATTRIBUTE_ALIAS omitted: Mach-O does not support symbol aliases
# - HAVE_ZLIB, HAVE_ZSTD, HAVE_COMPRESSION omitted: shader cache disabled; iOS SDK lacks libzstd
# - _GNU_SOURCE omitted: BSD/Darwin headers
# - Linux-only syscalls (memfd_create, getrandom, futex, inotify) omitted
set(MESA_DEFS_DARWIN
  ${MESA_DEFS_COMMON}
  -D_DARWIN_C_SOURCE
)

# Linux specific (glibc / Linux kernel facilities):
set(MESA_DEFS_LINUX
  ${MESA_DEFS_COMMON}
  -D_GNU_SOURCE
  -DHAVE_REALLOCARRAY
  -DHAVE_SC_PHYS_PAGES
  -DMAJOR_IN_SYSMACROS
  -DHAS_SCHED_H
  -DHAS_SCHED_GETAFFINITY
  -DHAVE_LINUX_FUTEX_H
  -DHAVE_SYS_INOTIFY_H
  -DHAVE_SYS_SHM_H
  -DHAVE_ENDIAN_H
  -DHAVE_MEMFD_CREATE
  -DHAVE_GETRANDOM
  -DHAVE_POSIX_FALLOCATE
  -DHAVE_SECURE_GETENV
  -DHAVE_MALLINFO2
  -DHAVE_GNU_QSORT_R
  -DHAVE_PROGRAM_INVOCATION_NAME
  -DHAVE_PTHREAD_SETAFFINITY
  -DHAVE_DL_ITERATE_PHDR
  -DHAVE_ISSIGNALING
  -DHAVE_RANDOM_R
)

# GCC-specific additions on Linux
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  list(APPEND MESA_DEFS_LINUX
    -DHAVE_FUNC_ATTRIBUTE_ALIAS
    -DHAVE_FUNC_ATTRIBUTE_OPTIMIZE
    -DHAVE___BUILTIN_ADD_OVERFLOW_P
    -DHAVE___BUILTIN_SUB_OVERFLOW_P
  )
endif()

# Select active define set based on target OS
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(MM_GLOBAL_DEFS ${MESA_DEFS_DARWIN})
else()
  set(MM_GLOBAL_DEFS ${MESA_DEFS_LINUX})
endif()

# Note: PAN_ARCH=11 is intentionally NOT in MM_GLOBAL_DEFS.
# Per-architecture libraries (mm_pan_arch_v11, mm_pan_format_v11, mm_pandecode_v11)
# receive -DPAN_ARCH=11 via mesa_sources.cmake (${lib}_DEFS).
# Arch-independent libraries (such as mm_pan_lib) must NOT have PAN_ARCH defined
# so that gen_macros.h includes common_pack.h (needed for pan_samples.c).
# Downstream consumers targeting arch 11 receive -DPAN_ARCH=11 via mesa_defs.txt/mk
# or target-specific definitions (e.g. mali_mesa_smoke).
list(APPEND MM_GLOBAL_DEFS
  -DWITH_PANFROST_DISASSEMBLE
)

# Emit mesa_defs.txt and mesa_defs.mk for downstream consumers (plugin, CI, Makefile)
function(mm_export_defs_files out_dir)
  file(MAKE_DIRECTORY "${out_dir}")
  
  # 1. Plain text format for shell scripts (Darwin / iOS flags)
  string(REPLACE ";" " " _txt_darwin "${MESA_DEFS_DARWIN} -DPAN_ARCH=11 -DWITH_PANFROST_DISASSEMBLE")
  file(WRITE "${out_dir}/mesa_defs.txt" "${_txt_darwin}\n")

  # 2. Makefile format
  set(_mk "# SPDX-License-Identifier: MIT\n# GENERATED from cmake/mesa_defs.cmake. Do not edit.\n")
  string(APPEND _mk "UNAME_S := $(shell uname -s)\n\n")
  
  string(APPEND _mk "MESA_DEFS_COMMON := \\\n")
  foreach(d ${MESA_DEFS_COMMON})
    string(APPEND _mk "  ${d} \\\n")
  endforeach()
  string(APPEND _mk "  -DPAN_ARCH=11 \\\n  -DWITH_PANFROST_DISASSEMBLE\n\n")

  string(APPEND _mk "MESA_DEFS_DARWIN := \\\n  $(MESA_DEFS_COMMON) \\\n  -D_DARWIN_C_SOURCE\n\n")

  string(APPEND _mk "MESA_DEFS_LINUX := \\\n  $(MESA_DEFS_COMMON) \\\n")
  foreach(d ${MESA_DEFS_LINUX})
    if(NOT d IN_LIST MESA_DEFS_COMMON)
      string(APPEND _mk "  ${d} \\\n")
    endif()
  endforeach()
  string(APPEND _mk "\n\nifeq ($(UNAME_S),Darwin)\n  MESA_DEFS := $(MESA_DEFS_DARWIN)\nelse\n  MESA_DEFS := $(MESA_DEFS_LINUX)\nendif\n")
  file(WRITE "${out_dir}/mesa_defs.mk" "${_mk}")
endfunction()

# Export in binary dir and source tree
mm_export_defs_files("${CMAKE_CURRENT_BINARY_DIR}")
mm_export_defs_files("${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT MESA_BUILD_TREE AND DEFINED ENV{MESA_BUILD_TREE})
  set(MESA_BUILD_TREE "$ENV{MESA_BUILD_TREE}")
endif()
if(MESA_BUILD_TREE AND EXISTS "${MESA_BUILD_TREE}")
  mm_export_defs_files("${MESA_BUILD_TREE}")
endif()

# ABI Compile-Time Check:
# Ensure HAVE_FUNC_ATTRIBUTE_PACKED is set and pan_varying_layout size matches 152 bytes.
include(CheckCSourceCompiles)
set(_saved_flags "${CMAKE_REQUIRED_FLAGS}")
set(_saved_defs "${CMAKE_REQUIRED_DEFINITIONS}")
set(CMAKE_REQUIRED_FLAGS "-Werror")
set(CMAKE_REQUIRED_DEFINITIONS ${MM_GLOBAL_DEFS})
check_c_source_compiles("
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>
  #ifndef HAVE_FUNC_ATTRIBUTE_PACKED
  #error \"ABI requirement violated: HAVE_FUNC_ATTRIBUTE_PACKED must be defined\"
  #endif
  enum __attribute__((packed)) pan_test_knowledge {
     PAN_VARYING_FORMAT_KNOWN = 1,
     PAN_VARYING_LAYOUT_KNOWN = 2,
  };
  struct pan_test_slot {
     uint32_t location : 7;
     unsigned alu_type : 8;
     unsigned ncomps : 3;
     int section : 2;
     int offset : 12;
  };
  _Static_assert(sizeof(struct pan_test_slot) == 4, \"slot size 4\");
  struct pan_varying_layout_test {
     uint8_t count;
     enum pan_test_knowledge known;
     uint16_t generic_size_B;
     struct pan_test_slot slots[37];
  };
  _Static_assert(sizeof(struct pan_varying_layout_test) == 152, \"layout size 152\");
  _Static_assert(offsetof(struct pan_varying_layout_test, count) == 0, \"count offset 0\");
  _Static_assert(offsetof(struct pan_varying_layout_test, known) == 1, \"known offset 1\");
  _Static_assert(offsetof(struct pan_varying_layout_test, generic_size_B) == 2, \"generic_size_B offset 2\");
  _Static_assert(offsetof(struct pan_varying_layout_test, slots) == 4, \"slots offset 4\");
  int main(void) { return 0; }
" MM_ABI_ASSERTION_VERIFIED)
set(CMAKE_REQUIRED_FLAGS "${_saved_flags}")
set(CMAKE_REQUIRED_DEFINITIONS "${_saved_defs}")

if(NOT MM_ABI_ASSERTION_VERIFIED)
  message(FATAL_ERROR "Compile-time ABI assertion failed: struct pan_varying_layout must be 152 bytes with HAVE_FUNC_ATTRIBUTE_PACKED")
endif()
