# SPDX-License-Identifier: MIT
# Mali-G715 (Valhall CSF, PAN_ARCH 11) Mesa library set for the Metal plug-in / host tools.
#
# Static libraries only, no DRM, no Vulkan runtime, no GL, no LLVM. Spec 06 s4 (what to
# compile), s8 (iOS build concerns). ORIGINAL build glue: Mesa (MIT) sources are compiled from a
# separate checkout (${MESA_SRC}); nothing of Mesa is copied into this tree.
#
# Two inputs besides this directory:
#   MESA_SRC  full Mesa checkout, revision pinned to 1f9035d9c04ca391a58155159027b1d85fba8be8
#   MESA_GEN  generated sources/headers staged by scripts/host-gen.sh (python generators:
#             genxml v11_pack.h, NIR, bifrost/valhall tables, u_format; need mako)
#
# Targets:
#   mm_*                 one static library per Mesa meson library (names in cmake/mesa_sources.cmake)
#   mali_mesa_headers    INTERFACE: include dirs + defines a consumer needs (cs_builder.h, v11_pack.h,
#                        pan_*.h, nir.h, xf86drm.h shim).
#   mali_mesa            INTERFACE: links every library of the set (link-order independent on Apple ld64 only).
#   mali_mesa_combined   custom target: ONE archive libmali_mesa.a (all objects, cycles resolved)
#
# Toolchains:
#   iOS:   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ios-arm64.toolchain.cmake \
#                -DMESA_SRC=... -DMESA_GEN=...
#   proxy: cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ios-arm64.toolchain.cmake \
#                -DMALI_PROXY_BUILD=ON -DMESA_SRC=... -DMESA_GEN=...
cmake_minimum_required(VERSION 3.24)
project(mali_mesa LANGUAGES C CXX)

set(MM_PINNED_MESA_REV "1f9035d9c04ca391a58155159027b1d85fba8be8")

set(MESA_SRC "" CACHE PATH "Full Mesa source tree")
set(MESA_GEN "" CACHE PATH "Staged generated files from scripts/host-gen.sh")
if(NOT DEFINED MESA_OUT_LIB AND DEFINED ENV{MESA_OUT_LIB})
  set(MESA_OUT_LIB "$ENV{MESA_OUT_LIB}" CACHE PATH "Optional destination directory for libmali_mesa.a")
else()
  set(MESA_OUT_LIB "${MESA_OUT_LIB}" CACHE PATH "Optional destination directory for libmali_mesa.a")
endif()
if(NOT DEFINED MESA_BUILD_TREE AND DEFINED ENV{MESA_BUILD_TREE})
  set(MESA_BUILD_TREE "$ENV{MESA_BUILD_TREE}" CACHE PATH "Optional destination directory for generated headers")
else()
  set(MESA_BUILD_TREE "${MESA_BUILD_TREE}" CACHE PATH "Optional destination directory for generated headers")
endif()
option(MALI_MESA_WITH_DECODE "Also build pandecode (CS/descriptor dumper, debug aid)" OFF)
option(MALI_MESA_ASSERTS     "Keep assert() (drops -DNDEBUG). cs_builder asserts on bad register use" OFF)
set(MALI_MESA_LIBPAN_DIR "" CACHE PATH "Output dir of scripts/host-libpan.sh (precompiled libpan kernels); optional")
option(MALI_MESA_BUILD_SMOKE "Build tests/smoke.c against the combined set" ON)

if(NOT EXISTS "${MESA_SRC}/meson.build")
  message(FATAL_ERROR "MESA_SRC='${MESA_SRC}' is not a full Mesa tree (missing meson.build)")
endif()
if(NOT EXISTS "${MESA_GEN}/MESA_REV.txt")
  message(FATAL_ERROR "MESA_GEN='${MESA_GEN}' missing; run scripts/host-gen.sh first")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(mesa_sources)

# Revision pin enforcement: fatal error on hash mismatch
file(READ "${MESA_SRC}/VERSION" _mesa_ver)
string(STRIP "${_mesa_ver}" _mesa_ver)
if(NOT _mesa_ver STREQUAL MM_MESA_VERSION)
  message(FATAL_ERROR "Mesa tree version '${_mesa_ver}' does not match expected '${MM_MESA_VERSION}'")
endif()

file(STRINGS "${MESA_GEN}/MESA_REV.txt" _gen_rev_lines)
list(GET _gen_rev_lines 0 _gen_git_hash)
string(STRIP "${_gen_git_hash}" _gen_git_hash)
if(NOT _gen_git_hash MATCHES "^1f9035d")
  message(FATAL_ERROR "MESA_GEN git revision '${_gen_git_hash}' does not match pinned revision ${MM_PINNED_MESA_REV}")
endif()

if(EXISTS "${MESA_SRC}/.git")
  execute_process(
    COMMAND git -C "${MESA_SRC}" rev-parse HEAD
    OUTPUT_VARIABLE _src_git_hash
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_res
  )
  if(_git_res EQUAL 0 AND NOT _src_git_hash MATCHES "^1f9035d")
    message(FATAL_ERROR "MESA_SRC git revision '${_src_git_hash}' does not match pinned revision ${MM_PINNED_MESA_REV}")
  endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # linked into a loadable bundle/dylib
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Canonical preprocessor defines & ABI compile-time assertion
include(mesa_defs)
include(mesa_config)   # -> MM_C_FLAGS, MM_CXX_FLAGS

if(MALI_MESA_ASSERTS)
  # CMake's Release/RelWithDebInfo/MinSizeRel flag sets carry -DNDEBUG themselves: strip it.
  foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
    foreach(_lang C CXX)
      string(REPLACE "-DNDEBUG" "" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
    endforeach()
  endforeach()
else()
  list(APPEND MM_GLOBAL_DEFS -DNDEBUG)
endif()

# ---- architecture -----------------------------------------------------------------------
include(CheckCSourceCompiles)
check_c_source_compiles("#ifndef __aarch64__\n#error not arm64\n#endif\nint main(void){return 0;}" MM_IS_AARCH64)
check_c_source_compiles("#ifndef __x86_64__\n#error not x86_64\n#endif\nint main(void){return 0;}" MM_IS_X86_64)

# ---- helpers ----------------------------------------------------------------------------
function(mm_prefix out root)
  set(_l "")
  foreach(f ${ARGN})
    list(APPEND _l "${root}/${f}")
  endforeach()
  set(${out} "${_l}" PARENT_SCOPE)
endfunction()

set(MM_SHIM_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/shims/include")

function(mm_inc_dirs out lib)
  set(_d "")
  foreach(f ${${lib}_INC_SRC})
    list(APPEND _d "${MESA_SRC}/${f}")
  endforeach()
  foreach(f ${${lib}_INC_GEN})
    list(APPEND _d "${MESA_GEN}/${f}")
  endforeach()
  set(${out} "${_d}" PARENT_SCOPE)
endfunction()

# ---- the libraries ------------------------------------------------------------------------
foreach(lib ${MM_LIBS})
  mm_prefix(_s "${MESA_SRC}" ${${lib}_SRC})
  mm_prefix(_g "${MESA_GEN}" ${${lib}_GEN})

  # per-arch replacements for x86-only pieces that make_manifest.py dropped
  if(lib STREQUAL "mm_blake3")
    if(MM_IS_AARCH64)
      list(APPEND _s "${MESA_SRC}/src/util/blake3/blake3_neon.c")
    else()
      # portable C only (the x86 .S set is dropped from the manifest)
      set(${lib}_DEFS ${${lib}_DEFS} -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512)
    endif()
  elseif(lib STREQUAL "mm_util")
    # CPU cache maintenance (cache_ops_aarch64.c reads CTR_EL0 and issues dc cvac/civac) is not
    # used by the arch-11 libraries; the kernel backend owns coherency. Null ops everywhere.
    list(APPEND _s "${MESA_SRC}/src/util/cache_ops_null.c")
  endif()

  if(lib MATCHES "^mm_pandecode" AND NOT MALI_MESA_WITH_DECODE)
    continue()
  endif()

  add_library(${lib} STATIC ${_s} ${_g})
  mm_inc_dirs(_inc ${lib})
  target_include_directories(${lib} PRIVATE "${MM_SHIM_INCLUDE}" ${_inc})
  target_compile_definitions(${lib} PRIVATE ${MM_GLOBAL_DEFS} ${${lib}_DEFS})
  target_compile_options(${lib} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${MM_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:CXX>:${MM_CXX_FLAGS}>)
  set_target_properties(${lib} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    OUTPUT_NAME "${lib}")
  list(APPEND MM_BUILT_LIBS ${lib})
endforeach()

# ---- optional: precompiled libpan kernels (spec 06 s4 P8)
if(MALI_MESA_LIBPAN_DIR AND EXISTS "${MALI_MESA_LIBPAN_DIR}/libpan_shaders_v11.c")
  add_library(mm_libpan_v11 STATIC "${MALI_MESA_LIBPAN_DIR}/libpan_shaders_v11.c")
  mm_inc_dirs(_lp_inc mm_pan_arch_v11)
  target_include_directories(mm_libpan_v11 PRIVATE "${MALI_MESA_LIBPAN_DIR}" "${MM_SHIM_INCLUDE}" ${_lp_inc})
  target_compile_definitions(mm_libpan_v11 PRIVATE ${MM_GLOBAL_DEFS} PAN_ARCH=11)
  target_compile_options(mm_libpan_v11 PRIVATE $<$<COMPILE_LANGUAGE:C>:${MM_C_FLAGS}>)
  set_target_properties(mm_libpan_v11 PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  list(APPEND MM_BUILT_LIBS mm_libpan_v11)
endif()

# ---- consumer-facing targets ----------------------------------------------------------------
add_library(mali_mesa_headers INTERFACE)
mm_inc_dirs(_hdr_inc mm_pan_arch_v11)
target_include_directories(mali_mesa_headers INTERFACE "${MM_SHIM_INCLUDE}" ${_hdr_inc})
target_compile_definitions(mali_mesa_headers INTERFACE ${MM_GLOBAL_DEFS})

# Note on link order:
# Apple ld64 resolves static archives order-independently (multi-pass resolution).
# GNU ld / bfd ld requires --start-group ... --end-group because util, nir, and compiler form cycles.
# For non-Apple consumers, always link the combined archive libmali_mesa.a.
add_library(mali_mesa INTERFACE)
target_link_libraries(mali_mesa INTERFACE mali_mesa_headers ${MM_BUILT_LIBS})
if(NOT APPLE)
  target_link_libraries(mali_mesa INTERFACE m pthread)
endif()

# One archive, all objects: Mesa's libraries reference each other in cycles (util <-> nir <->
# compiler), and iOS plug-in link lines are easier with one -l.
# Duplicate object basenames:
#   - pan_blend.c.o (in mm_pan_arch_v11 and mm_pan_lib)
#   - disassemble.c.o (in mm_pan_bifrost_disasm and mm_pan_midgard_disasm)
#   - midgard_ops.c.o (in mm_pan_midgard and mm_pan_midgard_disasm)
# These are cleanly preserved by ar MRI scripts on Linux and accepted by Apple libtool -static.
set(MM_COMBINED "${CMAKE_BINARY_DIR}/lib/libmali_mesa.a")
set(_comb_inputs "")
foreach(lib ${MM_BUILT_LIBS})
  list(APPEND _comb_inputs "$<TARGET_FILE:${lib}>")
endforeach()

if(APPLE)
  if(NOT CMAKE_LIBTOOL)
    find_program(CMAKE_LIBTOOL NAMES libtool llvm-libtool-darwin REQUIRED)
  endif()
  add_custom_command(OUTPUT "${MM_COMBINED}"
    COMMAND "${CMAKE_LIBTOOL}" -static -o "${MM_COMBINED}" ${_comb_inputs}
    DEPENDS ${MM_BUILT_LIBS} VERBATIM COMMAND_EXPAND_LISTS)
else()
  list(JOIN _comb_inputs "|" _comb_joined)   # one argv entry; combine_archives.cmake splits on |
  add_custom_command(OUTPUT "${MM_COMBINED}"
    COMMAND "${CMAKE_COMMAND}" -DAR=${CMAKE_AR} -DOUT=${MM_COMBINED}
            "-DINPUTS=${_comb_joined}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/combine_archives.cmake"
    DEPENDS ${MM_BUILT_LIBS} VERBATIM)
endif()
add_custom_target(mali_mesa_combined ALL DEPENDS "${MM_COMBINED}")

# Support output directly to MESA_OUT_LIB if specified
if(NOT MESA_OUT_LIB AND DEFINED ENV{MESA_OUT_LIB})
  set(MESA_OUT_LIB "$ENV{MESA_OUT_LIB}")
endif()
if(MESA_OUT_LIB)
  add_custom_command(TARGET mali_mesa_combined POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MESA_OUT_LIB}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MM_COMBINED}" "${MESA_OUT_LIB}/libmali_mesa.a"
    COMMENT "Copying libmali_mesa.a to ${MESA_OUT_LIB}"
  )
endif()

if(MALI_MESA_BUILD_SMOKE)
  if(CMAKE_CROSSCOMPILING)
    # Cross builds (iOS / aarch64 proxy) cannot run the test, and the proxy box has no aarch64
    # libstdc++ to link with: compile it to prove the headers + PAN_ARCH=11 + defines work, and
    # stop there. Link/run it for real on the device (or macOS) with the combined archive.
    add_library(mali_mesa_smoke OBJECT tests/smoke.c)
    target_include_directories(mali_mesa_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(mali_mesa_smoke PRIVATE mali_mesa_headers)
  else()
    # Link against the combined archive: Mesa's libraries reference each other in cycles, and a
    # single archive is what the plug-in will link too (so this also proves that link line).
    add_executable(mali_mesa_smoke tests/smoke.c)
    add_dependencies(mali_mesa_smoke mali_mesa_combined)
    target_include_directories(mali_mesa_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(mali_mesa_smoke PRIVATE mali_mesa_headers "${MM_COMBINED}")
    target_link_libraries(mali_mesa_smoke PRIVATE m pthread stdc++)
  endif()
  target_compile_definitions(mali_mesa_smoke PRIVATE PAN_ARCH=11)
  target_compile_options(mali_mesa_smoke PRIVATE ${MM_C_FLAGS})
endif()
