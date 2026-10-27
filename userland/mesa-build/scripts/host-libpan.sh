#!/usr/bin/env bash
# OPTIONAL stage (spec 06 s4 P8, milestone M2): precompiled compute kernels (panlib_fill,
# panlib_copy_mem, indirect-dispatch, query pool, draw helpers) as plain C data.
#
# libpan/*.cl -> SPIR-V (mesa_clc: LLVM+clang+libclc+SPIRV-LLVM-Translator+SPIRV-Tools) ->
# panfrost_compile (built from libpanfrost_compiler, runs on the host) -> libpan_shaders_v11.{h,c}.
# Only this step needs LLVM; the iOS side links the generated .c and has no LLVM at run time
# (spec 06 s8 item 6). NOT RUN on the dev box: libclc / SPIRV-Tools / LLVMSPIRVLib are not
# installed there (checked: no libclc, no spirv-opt, no LLVMSPIRVLib headers).
#
# Needs (Debian/Ubuntu): libclang-dev libllvmspirvlib-dev spirv-tools libclc-dev llvm-dev
#                        python3-mako (+ glslang for vulkan runtime, unused here)
# Output: $WORK/libpan/{libpan_shaders_v11.h,libpan_shaders_v11.c}; point the CMake build at it with
#         -DMALI_MESA_LIBPAN_DIR=$WORK/libpan   (adds target mm_libpan_v11 to the combined archive)
set -euo pipefail
. "$(dirname "$0")/env.sh"
mali_mesa_ensure_venv
export PATH="$VENV/bin:$PATH"
B="$WORK/build-libpan"
# libpan is built when with_panfrost_vk (vulkan-drivers=panfrost): pulls vulkan runtime + mesa_clc.
[ -f "$B/build.ninja" ] || meson setup "$B" "$MESA_SRC" -Dbuildtype=release -Dllvm=enabled \
  -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dmesa-clc=enabled \
  -Dprecomp-compiler=enabled -Dbuild-tests=false -Dpanfrost-rust=false
ninja -C "$B" src/panfrost/libpan/libpan_shaders_v11.h src/panfrost/libpan/libpan_shaders_v11.c
mkdir -p "$WORK/libpan"
cp "$B"/src/panfrost/libpan/libpan_shaders_v11.[hc] "$WORK/libpan/"
echo "libpan v11 kernels at $WORK/libpan"
