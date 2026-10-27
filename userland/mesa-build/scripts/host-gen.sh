#!/usr/bin/env bash
# Stage 1 (host, any OS with python3): run Mesa's own meson once, NATIVELY, to
#   (a) execute every code generator (genxml gen_pack.py -> v11_pack.h, Mako/NIR/bifrost/valhall
#       generators, u_format tables, ...), and
#   (b) record the exact compile graph in compile_commands.json,
# then stage ONLY the generated files that the arch-11 library set consumes into $MESA_GEN.
#
# Why a native meson run and not a meson cross build: Mesa's generators are host python
# scripts, the iOS toolchain only needs the *outputs*, and no iOS SDK exists on the Linux box
# (the SDK lives on the macOS runner, 00-implementation-plan.md:127). The target-specific build
# is then CMakeLists.txt (stage 3).
set -euo pipefail
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"
mali_mesa_ensure_venv
export PATH="$VENV/bin:$PATH"

[ -f "$MESA_SRC/meson.build" ] || { echo "MESA_SRC=$MESA_SRC is not a full Mesa tree" >&2; exit 1; }

if [ ! -f "$HOST_BUILD/build.ninja" ]; then
  mkdir -p "$HOST_BUILD"
  meson setup "$HOST_BUILD" "$MESA_SRC" \
    -Dbuildtype=release \
    -Dtools=panfrost \
    -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dvulkan-layers= \
    -Dopengl=true -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
    -Dgallium-va=disabled -Dgallium-rusticl=false -Dpanfrost-rust=false \
    -Dshader-cache=disabled -Dzstd=disabled -Dzlib=disabled \
    -Dexpat=disabled -Dxmlconfig=disabled
fi

# The meson static-library targets that make up the arch-11 set (see make_manifest.py LIBS).
TARGETS="$(python3 "$HERE/scripts/make_manifest.py" --print-ninja-targets)"
# shellcheck disable=SC2086
ninja -C "$HOST_BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" $TARGETS

# Stage the generated closure: every file the ninja deps log shows under the build dir that is
# not an object/archive. `ninja -t deps` is exact (it is the compiler's own -MD output).
mkdir -p "$MESA_GEN"
{
  ninja -C "$HOST_BUILD" -t deps | sed -n 's/^[[:space:]]\+//p'
  # generated .c/.cpp translation units are not "deps" of anything: take them from the compile db
  python3 "$HERE/scripts/make_manifest.py" --print-generated-sources "$HOST_BUILD"
} | python3 "$HERE/scripts/stage_gen.py" "$HOST_BUILD" "$MESA_GEN"

# Record provenance so the manifest is tied to a Mesa revision.
( cd "$MESA_SRC" && { git rev-parse HEAD 2>/dev/null || echo unknown; cat VERSION; } ) > "$MESA_GEN/MESA_REV.txt"
if [ ! -f "$HERE/sources/generated-files.txt" ] || [ "${1:-}" = "--update-manifest" ]; then
  mkdir -p "$HERE/sources"
  ( cd "$MESA_GEN" && find . -type f ! -name MESA_REV.txt | sed 's|^\./||' | sort ) > "$HERE/sources/generated-files.txt"
fi
echo "ok: generated tree at $MESA_GEN ($(cat "$MESA_GEN/MESA_REV.txt" | tr '\n' ' '))"
