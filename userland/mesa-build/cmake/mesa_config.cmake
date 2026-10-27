# Replacement for Mesa's meson `pre_args` (the global -DHAVE_* set), probed against the TARGET
# toolchain instead of inherited from the Linux host that ran the generators.
#
# Mesa's meson derives these in meson.build:1000-1360 via cc.has_function()/has_header()/
# compiles(). Doing the same with check_symbol_exists keeps the iOS build honest: on the host
# meson run HAVE_LIBDRM, HAVE_MEMFD_CREATE, HAVE_GETRANDOM, HAVE_LINUX_FUTEX_H ... were all 1 and
# must NOT leak into an iOS object (they select Linux-only paths: spec 06 s8 items 4,9).
#
# Result variables:
#   MM_GLOBAL_DEFS   list of -D tokens applied to every Mesa translation unit
#   MM_C_FLAGS / MM_CXX_FLAGS  flag lists

include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckCSourceCompiles)

if(NOT DEFINED MM_GLOBAL_DEFS)
  include(mesa_defs)
endif()
set(_mm_probe_defs "")

# Linux needs _GNU_SOURCE for most of the probed symbols and Mesa defines it there
# (meson.build:1313-1316). Darwin/iOS must NOT define it; Darwin needs _DARWIN_C_SOURCE.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND MM_GLOBAL_DEFS -D_GNU_SOURCE)
  set(_mm_probe_defs -D_GNU_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  list(APPEND MM_GLOBAL_DEFS -D_DARWIN_C_SOURCE)
  set(_mm_probe_defs -D_DARWIN_C_SOURCE)
endif()
set(CMAKE_REQUIRED_DEFINITIONS ${_mm_probe_defs})

# mm_probe(<define> <symbol> <header...>)
function(mm_probe def sym)
  set(var "MM_PROBE_${def}")
  check_symbol_exists("${sym}" "${ARGN}" ${var})
  if(${var})
    set(MM_GLOBAL_DEFS ${MM_GLOBAL_DEFS} -D${def} PARENT_SCOPE)
  endif()
endfunction()

function(mm_probe_header def hdr)
  set(var "MM_PROBE_${def}")
  check_include_file("${hdr}" ${var})
  if(${var})
    set(MM_GLOBAL_DEFS ${MM_GLOBAL_DEFS} -D${def} PARENT_SCOPE)
  endif()
endfunction()

# ---- libc surface (all probed) -------------------------------------------------------------
mm_probe(HAVE_FMEMOPEN        fmemopen        stdio.h)
mm_probe(HAVE_POSIX_MEMALIGN  posix_memalign  stdlib.h)
mm_probe(HAVE_STRTOK_R        strtok_r        string.h)
mm_probe(HAVE_STRTOF          strtof          stdlib.h)
mm_probe(HAVE_REALLOCARRAY    reallocarray    stdlib.h)
mm_probe(HAVE_MKOSTEMP        mkostemp        stdlib.h)
mm_probe(HAVE_SECURE_GETENV   secure_getenv   stdlib.h)
mm_probe(HAVE_RANDOM_R        random_r        stdlib.h)
mm_probe(HAVE_FLOCK           flock           sys/file.h)
mm_probe(HAVE_SYSCONF         sysconf         unistd.h)
mm_probe(HAVE_GETRANDOM       getrandom       sys/random.h)
mm_probe(HAVE_MEMFD_CREATE    memfd_create    sys/mman.h)
mm_probe(HAVE_POSIX_FALLOCATE posix_fallocate fcntl.h)
mm_probe(HAVE_DLADDR          dladdr          dlfcn.h)
mm_probe(HAVE_MALLINFO2       mallinfo2       malloc.h)
mm_probe(HAVE_ISSIGNALING     issignaling     math.h)
mm_probe(HAVE_PTHREAD_SETAFFINITY pthread_setaffinity_np pthread.h)
mm_probe(HAS_SCHED_GETAFFINITY sched_getaffinity sched.h)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR APPLE)
  # strtod_l lives in xlocale.h on Darwin
  mm_probe(HAVE_STRTOD_L      strtod_l        stdlib.h xlocale.h)
else()
  mm_probe(HAVE_STRTOD_L      strtod_l        stdlib.h)
endif()

mm_probe_header(HAVE_DLFCN_H        dlfcn.h)
mm_probe_header(HAVE_POLL_H         poll.h)
mm_probe_header(HAVE_ENDIAN_H       endian.h)
mm_probe_header(HAVE_LINUX_FUTEX_H  linux/futex.h)
mm_probe_header(HAVE_SYS_INOTIFY_H  sys/inotify.h)
mm_probe_header(HAVE_SYS_SHM_H      sys/shm.h)
mm_probe_header(HAS_SCHED_H         sched.h)
mm_probe_header(HAVE_CET_H          cet.h)
if(MM_PROBE_HAVE_CET_H AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86|AMD64|amd64")
  list(REMOVE_ITEM MM_GLOBAL_DEFS -DHAVE_CET_H)
endif()

check_symbol_exists(dl_iterate_phdr link.h MM_PROBE_DL_ITERATE_PHDR)
if(MM_PROBE_DL_ITERATE_PHDR)
  list(APPEND MM_GLOBAL_DEFS -DHAVE_DL_ITERATE_PHDR)
endif()
check_symbol_exists(program_invocation_name errno.h MM_PROBE_PIN)
if(MM_PROBE_PIN)
  list(APPEND MM_GLOBAL_DEFS -DHAVE_PROGRAM_INVOCATION_NAME)
endif()
if(NOT (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS"))
  check_symbol_exists(_SC_PHYS_PAGES unistd.h MM_PROBE_SC_PHYS_PAGES)
  if(MM_PROBE_SC_PHYS_PAGES)
    list(APPEND MM_GLOBAL_DEFS -DHAVE_SC_PHYS_PAGES)
  endif()
endif()
check_symbol_exists(major sys/sysmacros.h MM_PROBE_MAJOR_SYSMACROS)
if(MM_PROBE_MAJOR_SYSMACROS)
  list(APPEND MM_GLOBAL_DEFS -DMAJOR_IN_SYSMACROS)
endif()

# GNU qsort_r (comparator last) vs BSD qsort_r (thunk first): Darwin is BSD, Mesa falls back to
# its own sort when HAVE_GNU_QSORT_R is unset (util/u_qsort.*).
check_c_source_compiles("
  #define _GNU_SOURCE
  #include <stdlib.h>
  static int cmp(const void *a, const void *b, void *c) { (void)a; (void)b; (void)c; return 0; }
  int main(void) { int v[2] = {0, 1}; qsort_r(v, 2, sizeof(int), cmp, 0); return 0; }"
  MM_PROBE_GNU_QSORT_R)
if(MM_PROBE_GNU_QSORT_R)
  list(APPEND MM_GLOBAL_DEFS -DHAVE_GNU_QSORT_R)
endif()

unset(CMAKE_REQUIRED_DEFINITIONS)

# __builtin_{add,sub}_overflow_p are GCC-only (clang lacks them). The generated
# nir_constant_expressions.c switches on these macros, and the host meson run used gcc, so the
# staged file is fine for both as long as the macros reflect the TARGET compiler.
check_c_source_compiles("int main(void){ int a = 1; return __builtin_add_overflow_p(a, 1, (int)0); }"
  MM_PROBE_ADD_OVERFLOW_P)
if(MM_PROBE_ADD_OVERFLOW_P)
  list(APPEND MM_GLOBAL_DEFS -DHAVE___BUILTIN_ADD_OVERFLOW_P)
endif()
check_c_source_compiles("int main(void){ int a = 1; return __builtin_sub_overflow_p(a, 1, (int)0); }"
  MM_PROBE_SUB_OVERFLOW_P)
if(MM_PROBE_SUB_OVERFLOW_P)
  list(APPEND MM_GLOBAL_DEFS -DHAVE___BUILTIN_SUB_OVERFLOW_P)
endif()

# ---- GCC function attributes: PROBED (meson.build:1271-1282 does cc.get_supported_function_attributes
# with werror). Not constant across targets: Mach-O has no `alias` ("aliases are not supported on
# darwin"), clang has no `optimize`. Defining a macro the target compiler rejects breaks the
# build, so each one is compile-tested with -Werror.
set(_mm_attr_decl_const            "int foo(void) __attribute__((const));")
set(_mm_attr_decl_flatten          "int foo(void) __attribute__((flatten));")
set(_mm_attr_decl_malloc           "void *foo(void) __attribute__((malloc));")
set(_mm_attr_decl_pure             "int foo(void) __attribute__((pure));")
set(_mm_attr_decl_unused           "int foo(void) __attribute__((unused));")
set(_mm_attr_decl_warn_unused_result "int foo(void) __attribute__((warn_unused_result));")
set(_mm_attr_decl_weak             "int foo(void) __attribute__((weak));")
set(_mm_attr_decl_format           "int foo(const char *p, ...) __attribute__((format(printf, 1, 2)));")
set(_mm_attr_decl_packed           "struct __attribute__((packed)) foo { int a; char b; };")
set(_mm_attr_decl_returns_nonnull  "int *foo(void) __attribute__((returns_nonnull));")
set(_mm_attr_decl_alias            "int foo(void) { return 0; } int bar(void) __attribute__((alias(\"foo\")));")
set(_mm_attr_decl_noreturn         "void foo(void) __attribute__((noreturn));")
set(_mm_attr_decl_optimize         "int foo(void) __attribute__((optimize(\"O3\")));")
set(_mm_attr_decl_cold             "void foo(void) __attribute__((cold));")
set(_mm_attr_decl_visibility       "int foo(void) __attribute__((visibility(\"hidden\")));")
set(_saved_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-Werror")
foreach(a const flatten malloc pure unused warn_unused_result weak format packed returns_nonnull
          alias noreturn optimize cold visibility)
  string(TOUPPER "${a}" _A)
  check_c_source_compiles("${_mm_attr_decl_${a}}\nint main(void){return 0;}" MM_PROBE_ATTR_${_A})
  if(MM_PROBE_ATTR_${_A})
    list(APPEND MM_GLOBAL_DEFS -DHAVE_FUNC_ATTRIBUTE_${_A})
  endif()
endforeach()
set(CMAKE_REQUIRED_FLAGS "${_saved_flags}")

# ---- things true on every clang/gcc target we support (meson checks them, all succeed) -------
list(APPEND MM_GLOBAL_DEFS
  -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DHAVE_DIRENT_D_TYPE -DHAVE_UINT128
  -DUSE_GCC_ATOMIC_BUILTINS
  -DHAVE___BUILTIN_ADD_OVERFLOW
  -DHAVE___BUILTIN_BSWAP32 -DHAVE___BUILTIN_BSWAP64 -DHAVE___BUILTIN_CLZ -DHAVE___BUILTIN_CLZLL
  -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_EXPECT -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL
  -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL -DHAVE___BUILTIN_TYPES_COMPATIBLE_P
  -DHAVE___BUILTIN_UNREACHABLE
  # real NIR, not nir_stub.c (u_stub_gfx_compute.h keys off this)
  -DHAVE_GFX_COMPUTE
  # fixed policy: library build, no GL/video/driver stack
  -DMESA_DEBUG=0 -DUSE_LIBGLVND=0 -DTHREAD_SANITIZER=0
  -DHAVE_OPENGL=0 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=0
  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
  -D_FILE_OFFSET_BITS=64
  -DWITH_PANFROST_DISASSEMBLE      # compiler debug dumps call disassemble_*; keep the disasm libs linked
  "-DPACKAGE_VERSION=\"${MM_MESA_VERSION}\""
)
list(REMOVE_DUPLICATES MM_GLOBAL_DEFS)
# Deliberately NOT defined (each selects a Linux/DRM-only path):
#   HAVE_LIBDRM MESA_SYSTEM_HAS_KMS_DRM ALLOW_KCMP ENABLE_SHADER_CACHE (disk_cache: fork/flock/ftw)
#   HAVE_LIBUDEV HAVE_LINUX_UDMABUF_H USE_LIBELF HAVE_OPENMP HAVE_RENDERDOC_INTEGRATION HAVE_SOFTPIPE
#   HAVE_SWRAST HAVE_LIBUNWIND

# ---- compiler flags -------------------------------------------------------------------------
set(MM_C_FLAGS
  -fno-math-errno -fno-trapping-math -fno-common -fvisibility=hidden
  -ffunction-sections -fdata-sections
  -Werror=implicit-function-declaration -Werror=return-type -Werror=incompatible-pointer-types
  -Werror=int-conversion -Werror=empty-body -Werror=format
  -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-variable
  -Wno-override-init -Wno-pointer-arith
)
set(MM_CXX_FLAGS
  -fno-math-errno -fno-trapping-math -fvisibility=hidden -ffunction-sections -fdata-sections
  -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-variable
)
